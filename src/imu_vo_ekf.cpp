#include <ros/ros.h>
#include <sensor_msgs/Imu.h>

#include <deque>
#include <iostream>

#include "common/utils.hpp"
#include "common/view.hpp"
#include "estimator/ekf.hpp"
#include "sensor/odom_6dof.hpp"

namespace cg {

ANGULAR_ERROR State::kAngError = ANGULAR_ERROR::LOCAL_ANGULAR_ERROR;

class EKFFusionNode {
 public:
  EKFFusionNode(ros::NodeHandle &nh, ros::NodeHandle &pnh) : viewer_(nh) {
    std::string topic_vo = "/odom_vo";
    std::string topic_imu = "/imu0";

    nh.getParam("topic_vo", topic_vo);
    nh.getParam("topic_imu", topic_imu);

    std::cout << "topic_vo: " << topic_vo << std::endl;
    std::cout << "topic_imu: " << topic_imu << std::endl;

    double acc_n, gyr_n, acc_w, gyr_w, sigma_pv, sigma_rp, sigma_yaw;
    nh.param("acc_noise", acc_n, 1e-2);
    nh.param("gyr_noise", gyr_n, 1e-4);
    nh.param("acc_bias_noise", acc_w, 1e-6);
    nh.param("gyr_bias_noise", gyr_w, 1e-8);

    nh.param("init_sigma_pv", sigma_pv, 0.01);
    nh.param("init_sigma_rp", sigma_rp, 0.01);
    nh.param("init_sigma_yaw", sigma_yaw, 5.0);

    sigma_rp *= kDegreeToRadian;
    sigma_yaw *= kDegreeToRadian;

    ekf_ptr_ = std::make_unique<EKF>(acc_n, gyr_n, acc_w, gyr_w);
    ekf_ptr_->state_ptr_->set_cov(sigma_pv, sigma_pv, sigma_rp, sigma_yaw, acc_w, gyr_w);

    imu_sub_ = nh.subscribe<sensor_msgs::Imu>(topic_imu, 10, boost::bind(&EKF::imu_callback, ekf_ptr_.get(), _1));
    vo_sub_ = nh.subscribe(topic_vo, 10, &EKFFusionNode::vo_callback, this);

    Tcb = Utils::getTransformEigen(pnh, "cam0/T_cam_imu");
  }

  ~EKFFusionNode() {}

  void vo_callback(const geometry_msgs::PoseWithCovarianceStampedConstPtr &vo_msg);

 private:
  ros::Subscriber imu_sub_;
  ros::Subscriber vo_sub_;

  Eigen::Isometry3d Tcb;
  Eigen::Isometry3d Tvw;

  EKFPtr ekf_ptr_;
  Viewer viewer_;
};

void EKFFusionNode::vo_callback(const geometry_msgs::PoseWithCovarianceStampedConstPtr &vo_msg) {
  Eigen::Vector3d vo_p;
  Eigen::Quaterniond vo_q;
  vo_p.x() = vo_msg->pose.pose.position.x;
  vo_p.y() = vo_msg->pose.pose.position.y;
  vo_p.z() = vo_msg->pose.pose.position.z;
  vo_q.x() = vo_msg->pose.pose.orientation.x;
  vo_q.y() = vo_msg->pose.pose.orientation.y;
  vo_q.z() = vo_msg->pose.pose.orientation.z;
  vo_q.w() = vo_msg->pose.pose.orientation.w;

  Eigen::Isometry3d Tvo;  // VO in frame V --> Tc0cn
  Tvo.linear() = vo_q.toRotationMatrix();
  Tvo.translation() = vo_p;

  const Eigen::Matrix<double, kMeasDim, kMeasDim> &R =
      Eigen::Map<const Eigen::Matrix<double, kMeasDim, kMeasDim>>(vo_msg->pose.covariance.data());

  if (!ekf_ptr_->inited_) {
    if (!ekf_ptr_->init(vo_msg->header.stamp.toSec())) return;

    Eigen::Isometry3d Tb0bm;
    Tb0bm.linear() = ekf_ptr_->state_ptr_->Rwb_;
    Tb0bm.translation().setZero();

    const Eigen::Isometry3d &Tc0cm = Tvo;

    Tvw = Tc0cm * Tcb * Tb0bm.inverse();  // c0 --> visual frame V, b0 --> world frame W

    printf("[cggos %s] System initialized.\n", __FUNCTION__);

    return;
  }

  // IEKF iteration update, same with EKF when n_ite = 1
  int n_ite = 10;
  Eigen::Matrix<double, kMeasDim, kStateDim> H_i;
  Eigen::Matrix<double, kStateDim, kMeasDim> K_i;
  for (int i = 0; i < n_ite; i++) {
    if (i == 0) *ekf_ptr_->state_ptr_i_ = *ekf_ptr_->state_ptr_;

    // x_i
    const Eigen::Isometry3d &Twb_i = ekf_ptr_->state_ptr_i_->pose();

    // measurement estimation h(x_i), Twb in frame V --> Tc0cn
    const Eigen::Isometry3d &Twb_in_V = Tvw * Twb_i * Tcb.inverse();

    // measurement jacobian H
    H_i = Odom6Dof::measurement_jacobi(vo_q, Twb_i, Tvw, Tcb);

    // for debug
    Odom6Dof::check_jacobian(vo_q, Twb_i, Tvw, Tcb);

    // residual = z - h(x_i)
    Eigen::Matrix<double, kMeasDim, 1> residual;
    residual.topRows(3) = Tvo.translation() - Twb_in_V.translation();
    residual.bottomRows(3) = State::rotation_residual(Tvo.linear(), Twb_in_V.linear());

    // residual -= H (x_prior - x_i)
    Eigen::Matrix<double, kStateDim, 1> delta_x = *ekf_ptr_->state_ptr_ - *ekf_ptr_->state_ptr_i_;
    residual -= H_i * delta_x;

    std::cout << "res: " << residual.transpose() << std::endl;

    ekf_ptr_->update_K(H_i, R, K_i);
    *ekf_ptr_->state_ptr_i_ = *ekf_ptr_->state_ptr_ + K_i * residual;
  }

  // update state and cov
  *ekf_ptr_->state_ptr_ = *ekf_ptr_->state_ptr_i_;
  ekf_ptr_->update_P(H_i, R, K_i);

  std::cout << "acc bias: " << ekf_ptr_->state_ptr_->acc_bias.transpose() << std::endl;
  std::cout << "gyr bias: " << ekf_ptr_->state_ptr_->gyr_bias.transpose() << std::endl;

  // view
  // for publish, Tvo in frame W --> Tb0bn
  Eigen::Isometry3d TvoB = Tvw.inverse() * Tvo * Tcb;
  viewer_.publish_vo(*ekf_ptr_->state_ptr_, TvoB);
}

}  // namespace cg

int main(int argc, char **argv) {
  ros::init(argc, argv, "imu_vo_fusion");

  ros::NodeHandle nh;
  ros::NodeHandle pnh("~");
  cg::EKFFusionNode fusion_node(nh, pnh);

  ros::spin();

  return 0;
}