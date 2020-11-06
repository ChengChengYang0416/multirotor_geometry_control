/*
 * Copyright 2015 Fadri Furrer, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Michael Burri, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Mina Kamel, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Janosch Nikolic, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Markus Achtelik, ASL, ETH Zurich, Switzerland
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rotors_control/lee_position_controller.h"
#include "rotors_control/tictoc.h"
#include <ctime>
#include <cstdlib>
#include <chrono>

namespace rotors_control
{

LeePositionController::LeePositionController()
	: initialized_params_(false),
	  controller_active_(false)
{
	InitializeParameters();
}

LeePositionController::~LeePositionController() {}

void LeePositionController::InitializeParameters()
{

	ROS_INFO("initialize");
	calculateAllocationMatrix(vehicle_parameters_.rotor_configuration_, &(controller_parameters_.allocation_matrix_));
	// To make the tuning independent of the inertia matrix we divide here.
	Last_R_des.setZero();
	Last_angular_rate_des.setZero();
	I.setZero();
	Inertia_hat <<  0.034,0,0,
	            0,0.045,0,
	            0,0,0.098;
	angular_acc_to_rotor_velocities_.resize(vehicle_parameters_.rotor_configuration_.rotors.size(), 4);

	theta_diag_hat << 0, 0, 0;
	theta_diag_hat_dot << 0, 0, 0;
	last_omega << 0, 0, 0;
	last_moment_control_input << 0, 0, 0;
	last_angular_velocity << 0, 0, 0;
	Y_diag_cl_integral_last << Eigen::Matrix3d::Zero();
	y_diag_cl_lower_case << Eigen::Matrix3d::Zero();
	M_integral << 0, 0, 0;
	M_integral_last << 0, 0, 0;
	M_bar << 0, 0, 0;
	mat_FIFO.resize(3, 1);
	index = 0;
	full = 0;
	ICL_N = 45;
	last_time = ros::Time().toSec();
	dt = 0.02;
	theta_m_hat << 0, 0, 0;
	theta_m_hat_dot << 0, 0, 0;

	theta_m_hat_R = 0;
	theta_m_hat_dot_R = 0;

	mat_mass_FIFO.resize(1, 1);
	y_m_cl_integral << Eigen::Vector3d::Zero();
	y_m_cl_integral_last << Eigen::Vector3d::Zero();
	y_m_cl_lower_case << Eigen::Vector3d::Zero();
	index_m = 0;
	full_m = 0;
	ICL_N_m = 20;


	initialized_params_ = true;
}

void LeePositionController::CalculateRotorVelocities(Eigen::VectorXd* rotor_velocities, geometry_msgs::Point* theta_esti, geometry_msgs::Point* theta_m_esti, nav_msgs::Odometry* error)
{
	assert(rotor_velocities);
	assert(initialized_params_);

	rotor_velocities->resize(vehicle_parameters_.rotor_configuration_.rotors.size());
	// Return 0 velocities on all rotors, until the first command is received.
	if (!controller_active_) {
		*rotor_velocities = Eigen::VectorXd::Zero(rotor_velocities->rows());
		return;
	}

	if (index >= ICL_N) {
		full = 1;
		index = 0;
	}
	index++;

	if (index_m >= ICL_N_m) {
		full_m = 1;
		index_m = 0;
	}
	index_m++;

	Eigen::Vector3d position_error;
	Eigen::Vector3d velocity_error;

	// compute b_3_d and the acceleration
	Eigen::Vector3d force_control_input;
	ComputeDesiredForce(&force_control_input, &position_error, &velocity_error);

	// compute angular acceleration and moment control input
	Eigen::Vector3d angular_acceleration;
	Eigen::Vector3d moment_control_input;
	ComputeDesiredAngularAcc(force_control_input, &angular_acceleration, &moment_control_input);
	(theta_esti->x) = theta_diag_hat(0);
	(theta_esti->y) = theta_diag_hat(1);
	(theta_esti->z) = theta_diag_hat(2);

	(theta_m_esti->x) = theta_m_hat_R;
	(theta_m_esti->y) = theta_m_hat_R;
	(theta_m_esti->z) = theta_m_hat_R;

	error->pose.pose.position.x = position_error(0);
	error->pose.pose.position.y = position_error(1);
	error->pose.pose.position.z = position_error(2);
	error->pose.pose.orientation.x = angle_error(0);
	error->pose.pose.orientation.y = angle_error(1);
	error->pose.pose.orientation.z = angle_error(2);
	error->pose.pose.orientation.w = Psi;
	error->twist.twist.linear.x = velocity_error(0);
	error->twist.twist.linear.y = velocity_error(1);
	error->twist.twist.linear.z = velocity_error(2);
	error->twist.twist.angular.x = angular_rate_error(0);
	error->twist.twist.angular.y = angular_rate_error(1);
	error->twist.twist.angular.z = angular_rate_error(2);

	// comput thrust control input and project thrust onto body z axis.
	double thrust = -force_control_input.dot(odometry_.orientation.toRotationMatrix().col(2));

	// this block use angular acceleration control input to compute the rotor velocities of every rotor
	/*
	// [4, 1] vector for angular acceleration and thrust
	Eigen::Vector4d angular_acceleration_thrust;
	angular_acceleration_thrust.block<3, 1>(0, 0) = angular_acceleration;
	angular_acceleration_thrust(3) = thrust;

	// inertia matrix
	I.block<3, 3>(0, 0) = Inertia_hat;
	I(3, 3) = 1;

	// compute (inverse of allocation matrix)*(inertia matrix)
	angular_acc_to_rotor_velocities_ = controller_parameters_.allocation_matrix_.transpose()* (controller_parameters_.allocation_matrix_* controller_parameters_.allocation_matrix_.transpose()).inverse() * I;

	// compute every rotor's velocities
	*rotor_velocities = angular_acc_to_rotor_velocities_ * angular_acceleration_thrust;
	*rotor_velocities = rotor_velocities->cwiseMax(Eigen::VectorXd::Zero(rotor_velocities->rows()));
	*rotor_velocities = rotor_velocities->cwiseSqrt();
	*/

	// this block use moment control input to compute the rotor velocities of every rotor
	// [4, 1] vector for moment and thrust
	Eigen::Vector4d moment_thrust;
	moment_thrust.block<3, 1>(0, 0) = moment_control_input;
	moment_thrust(3) = thrust;

	// inertia matrix
	I.block<3, 3>(0, 0) = Inertia_hat;
	I(3, 3) = 1;

	moment_thrust_to_rotor_velocities_ = controller_parameters_.allocation_matrix_.transpose()* (controller_parameters_.allocation_matrix_* controller_parameters_.allocation_matrix_.transpose()).inverse();
	*rotor_velocities = moment_thrust_to_rotor_velocities_ * moment_thrust;
	*rotor_velocities = rotor_velocities->cwiseMax(Eigen::VectorXd::Zero(rotor_velocities->rows()));
	*rotor_velocities = rotor_velocities->cwiseSqrt();

}

void LeePositionController::SetOdometry(const EigenOdometry& odometry)
{
	odometry_ = odometry;
}

void LeePositionController::SetTrajectoryPoint(const mav_msgs::EigenTrajectoryPoint& command_trajectory)
{
	command_trajectory_ =  command_trajectory;
	controller_active_ = true;
}

void LeePositionController::ComputeDesiredForce(Eigen::Vector3d* force_control_input, Eigen::Vector3d* position_error, Eigen::Vector3d* velocity_error)
{
	assert(force_control_input);
	// this function is used to compute b_3_d in paper

	Eigen::Vector3d e_3(Eigen::Vector3d::UnitZ());

	// calculate position error ([3, 1] vector)
	*position_error = odometry_.position - command_trajectory_.position_W;

	// Transform velocity to world frame.
	// quaternion -> rotation matrix
	// compute velocity error in world frame
	const Eigen::Matrix3d R_W_I = odometry_.orientation.toRotationMatrix();
	Eigen::Vector3d velocity_W =  R_W_I * odometry_.velocity;
	*velocity_error = velocity_W - command_trajectory_.velocity_W;

	//connect the desired force with the acceleration command

	*force_control_input = (position_error->cwiseProduct(controller_parameters_.position_gain_)
	                        + velocity_error->cwiseProduct(controller_parameters_.velocity_gain_))
	                       - vehicle_parameters_.mass_ * vehicle_parameters_.gravity_ * e_3
	                       - vehicle_parameters_.mass_ * command_trajectory_.acceleration_W;


}

// Implementation from the T. Lee et al. paper
// Control of complex maneuvers for a quadrotor UAV using geometric methods on SE(3)
void LeePositionController::ComputeDesiredAngularAcc(const Eigen::Vector3d& force_control_input,
                Eigen::Vector3d* angular_acceleration, Eigen::Vector3d* moment_control_input)
{
	assert(angular_acceleration);

	// quaternion -> rotation matrix
	Eigen::Matrix3d R = odometry_.orientation.toRotationMatrix();

	// Get the desired rotation matrix.
	// b_1_d is the time derivative of desired trajectory
	Eigen::Vector3d b1_des;
	double yaw = atan2(  command_trajectory_.velocity_W(1),command_trajectory_.velocity_W(0) );
	if(yaw <0 ) {
		yaw+=6.28;
	}
	/*double yaw = command_trajectory_.getYaw();*/
	b1_des << cos(yaw), sin(yaw), 0;
	//b1_des << 1, 0, 0;
	//b1_des << cos(90*M_PI/180), sin(90*M_PI/180), 0;

	// b_3_d is calculated in ComputeDesiredForce()
	Eigen::Vector3d b3_des;
	b3_des = -force_control_input / force_control_input.norm();

	// b2_des = b3_des x b1_des
	Eigen::Vector3d b2_des;
	b2_des = b3_des.cross(b1_des);
	b2_des.normalize();

	// R_des = [b2_des x b3_des; b2_des; b3_des]
	Eigen::Matrix3d R_des;
	R_des.col(0) = b2_des.cross(b3_des);
	R_des.col(1) = b2_des;
	R_des.col(2) = b3_des;

	// Angle error according to lee et al.
	// use vectorFromSkewMatrix() to compute e_R and put it into angle_error
	Eigen::Matrix3d angle_error_matrix = 0.5 * (R_des.transpose() * R - R.transpose() * R_des);
	vectorFromSkewMatrix(angle_error_matrix, &angle_error);

	end = std::chrono::system_clock::now();
	std::chrono::duration<double> elapsed_seconds = end - start;
	double dt_tictoc = elapsed_seconds.count();
	//std::cout << "dt_tictoc = " << std::endl << dt_tictoc << std::endl;
	//double dt = 0.02;
	start = std::chrono::system_clock::now();


	Eigen::Matrix3d R_des_dot;
	R_des_dot = ( R_des-Last_R_des )/dt;
	Eigen::Matrix3d angular_rate_des_matrix = R_des.transpose() * R_des_dot;
	Eigen::Vector3d angular_rate_des;
	vectorFromSkewMatrix(angular_rate_des_matrix, &angular_rate_des);

	//Eigen::Vector3d angular_rate_des(Eigen::Vector3d::Zero());
	//angular_rate_des[2] = command_trajectory_.getYawRate();
	angular_rate_error = odometry_.angular_velocity - R.transpose() * R_des * angular_rate_des;

	k_R = controller_parameters_.attitude_gain_.transpose();
	k_omega = controller_parameters_.angular_rate_gain_.transpose();
	*moment_control_input = - angle_error.cwiseProduct(k_R)
	                        -angular_rate_error.cwiseProduct(k_omega)
	                        + odometry_.angular_velocity.cross(Inertia_hat*odometry_.angular_velocity);
}
}
