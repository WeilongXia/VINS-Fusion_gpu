/*******************************************************
 * Copyright (C) 2019, Aerial Robotics Group, Hong Kong University of Science and Technology
 *
 * This file is part of VINS.
 *
 * Licensed under the GNU General Public License v3.0;
 * you may not use this file except in compliance with the License.
 *
 * Author: Qin Tong (qintonguav@gmail.com)
 *******************************************************/

#include "estimator/estimator.h"
#include "estimator/parameters.h"
#include "utility/visualization.h"
#include <cv_bridge/cv_bridge.h>
#include <map>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <queue>
#include <ros/ros.h>
#include <stdio.h>
#include <thread>

Estimator estimator;

queue<sensor_msgs::ImuConstPtr> imu_buf;
queue<sensor_msgs::PointCloudConstPtr> feature_buf;
queue<sensor_msgs::ImageConstPtr> img0_buf;
queue<sensor_msgs::ImageConstPtr> img1_buf;
std::mutex m_buf;

bool first_receive_gt = true;
Eigen::Matrix4d gt_to_estimate;

void img0_callback(const sensor_msgs::ImageConstPtr &img_msg)
{
    m_buf.lock();
    img0_buf.push(img_msg);
    m_buf.unlock();
}

void img1_callback(const sensor_msgs::ImageConstPtr &img_msg)
{
    m_buf.lock();
    img1_buf.push(img_msg);
    m_buf.unlock();
}

cv::Mat getImageFromMsg(const sensor_msgs::ImageConstPtr &img_msg)
{
    cv_bridge::CvImageConstPtr ptr;
    // ROS_INFO("height: %d , width: %d", img_msg->height, img_msg->width);
    if (img_msg->encoding == "8UC1")
    {
        sensor_msgs::Image img;
        img.header = img_msg->header;
        img.height = img_msg->height;
        img.width = img_msg->width;
        img.is_bigendian = img_msg->is_bigendian;
        img.step = img_msg->step;
        img.data = img_msg->data;
        img.encoding = "mono8";
        ptr = cv_bridge::toCvCopy(img, sensor_msgs::image_encodings::MONO8);
    }
    else
        ptr = cv_bridge::toCvCopy(img_msg, sensor_msgs::image_encodings::MONO8);

    cv::Mat img = ptr->image.clone();
    return img;
}

// extract images with same timestamp from two topics
void sync_process()
{
    while (1)
    {
        if (STEREO)
        {
            cv::Mat image0, image1;
            std_msgs::Header header;
            double time = 0;
            m_buf.lock();
            if (!img0_buf.empty() && !img1_buf.empty())
            {
                double time0 = img0_buf.front()->header.stamp.toSec();
                double time1 = img1_buf.front()->header.stamp.toSec();
                if (time0 < time1)
                {
                    img0_buf.pop();
                    printf("throw img0\n");
                }
                else if (time0 > time1)
                {
                    img1_buf.pop();
                    printf("throw img1\n");
                }
                else
                {
                    time = img0_buf.front()->header.stamp.toSec();
                    header = img0_buf.front()->header;
                    image0 = getImageFromMsg(img0_buf.front());
                    img0_buf.pop();
                    image1 = getImageFromMsg(img1_buf.front());
                    img1_buf.pop();
                    // printf("find img0 and img1\n");
                }
            }
            m_buf.unlock();
            if (!image0.empty())
                estimator.inputImage(time, image0, image1);
        }
        else
        {
            cv::Mat image;
            std_msgs::Header header;
            double time = 0;
            m_buf.lock();
            if (!img0_buf.empty())
            {
                time = img0_buf.front()->header.stamp.toSec();
                header = img0_buf.front()->header;
                image = getImageFromMsg(img0_buf.front());
                img0_buf.pop();
            }
            m_buf.unlock();
            if (!image.empty())
                estimator.inputImage(time, image);
        }

        std::chrono::milliseconds dura(2);
        std::this_thread::sleep_for(dura);
    }
}

void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();
    double dx = imu_msg->linear_acceleration.x;
    double dy = imu_msg->linear_acceleration.y;
    double dz = imu_msg->linear_acceleration.z;
    double rx = imu_msg->angular_velocity.x;
    double ry = imu_msg->angular_velocity.y;
    double rz = imu_msg->angular_velocity.z;
    Vector3d acc(dx, dy, dz);
    Vector3d gyr(rx, ry, rz);
    estimator.inputIMU(t, acc, gyr);
    return;
}

void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
    map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> featureFrame;
    for (unsigned int i = 0; i < feature_msg->points.size(); i++)
    {
        int feature_id = feature_msg->channels[0].values[i];
        int camera_id = feature_msg->channels[1].values[i];
        double x = feature_msg->points[i].x;
        double y = feature_msg->points[i].y;
        double z = feature_msg->points[i].z;
        double p_u = feature_msg->channels[2].values[i];
        double p_v = feature_msg->channels[3].values[i];
        double velocity_x = feature_msg->channels[4].values[i];
        double velocity_y = feature_msg->channels[5].values[i];
        if (feature_msg->channels.size() > 5)
        {
            double gx = feature_msg->channels[6].values[i];
            double gy = feature_msg->channels[7].values[i];
            double gz = feature_msg->channels[8].values[i];
            pts_gt[feature_id] = Eigen::Vector3d(gx, gy, gz);
            // printf("receive pts gt %d %f %f %f\n", feature_id, gx, gy, gz);
        }
        ROS_ASSERT(z == 1);
        Eigen::Matrix<double, 7, 1> xyz_uv_velocity;
        xyz_uv_velocity << x, y, z, p_u, p_v, velocity_x, velocity_y;
        featureFrame[feature_id].emplace_back(camera_id, xyz_uv_velocity);
    }
    double t = feature_msg->header.stamp.toSec();
    estimator.inputFeature(t, featureFrame);
    return;
}

void restart_callback(const std_msgs::BoolConstPtr &restart_msg)
{
    if (restart_msg->data == true)
    {
        ROS_WARN("restart the estimator!");
        m_buf.lock();
        while (!feature_buf.empty())
            feature_buf.pop();
        while (!imu_buf.empty())
            imu_buf.pop();
        m_buf.unlock();
        estimator.clearState();
        estimator.setParameter();
    }
    return;
}

void mocap_callback(const geometry_msgs::PoseStamped::ConstPtr &mocap_msg_ptr)
{
    ofstream foutC(MOCAP_RESULT_PATH, ios::app);
    foutC.setf(ios::fixed, ios::floatfield);
    foutC.precision(9);
    foutC << mocap_msg_ptr->header.stamp.toSec() << ",";
    foutC.precision(5);
    foutC << mocap_msg_ptr->pose.position.x << "," << mocap_msg_ptr->pose.position.y << ","
          << mocap_msg_ptr->pose.position.z << "," << mocap_msg_ptr->pose.orientation.x << ","
          << mocap_msg_ptr->pose.orientation.y << "," << mocap_msg_ptr->pose.orientation.z << ","
          << mocap_msg_ptr->pose.orientation.w << endl;
    foutC.close();

    if (first_receive_gt)
    {
        Eigen::Matrix4d estimate_to_gt;
        estimate_to_gt.setIdentity(4, 4);
        Eigen::Quaterniond q(mocap_msg_ptr->pose.orientation.w, mocap_msg_ptr->pose.orientation.x,
                             mocap_msg_ptr->pose.orientation.y, mocap_msg_ptr->pose.orientation.z);
        estimate_to_gt.block<3, 3>(0, 0) = q.toRotationMatrix();
        estimate_to_gt.block<3, 1>(0, 3) << mocap_msg_ptr->pose.position.x, mocap_msg_ptr->pose.position.y,
            mocap_msg_ptr->pose.position.z;
        gt_to_estimate = estimate_to_gt.inverse();

        Eigen::Matrix4d gt_transformed = gt_to_estimate * estimate_to_gt;

        std::cout << "estimate_to_gt: " << estimate_to_gt << std::endl;
        std::cout << "gt_to_estimate: " << gt_to_estimate << std::endl;
        std::cout << "gt_transformed: " << gt_transformed << std::endl;

        Eigen::Quaterniond q_gt_estimate(gt_transformed.block<3, 3>(0, 0));

        nav_msgs::Odometry gt_pub;
        gt_pub.pose.pose.position.x = gt_transformed(0, 3);
        gt_pub.pose.pose.position.y = gt_transformed(1, 3);
        gt_pub.pose.pose.position.z = gt_transformed(2, 3);
        gt_pub.pose.pose.orientation.w = q_gt_estimate.w();
        gt_pub.pose.pose.orientation.x = q_gt_estimate.x();
        gt_pub.pose.pose.orientation.y = q_gt_estimate.y();
        gt_pub.pose.pose.orientation.z = q_gt_estimate.z();

        // publish mocap_msg_ptr aligned
        pubGt(gt_pub);

        first_receive_gt = false;
    }
    else
    {
        Eigen::Matrix4d estimate_to_gt;
        estimate_to_gt.setIdentity(4, 4);
        Eigen::Quaterniond q(mocap_msg_ptr->pose.orientation.w, mocap_msg_ptr->pose.orientation.x,
                             mocap_msg_ptr->pose.orientation.y, mocap_msg_ptr->pose.orientation.z);
        estimate_to_gt.block<3, 3>(0, 0) = q.toRotationMatrix();
        estimate_to_gt.block<3, 1>(0, 3) << mocap_msg_ptr->pose.position.x, mocap_msg_ptr->pose.position.y,
            mocap_msg_ptr->pose.position.z;

        Eigen::Matrix4d gt_transformed = gt_to_estimate * estimate_to_gt;

        Eigen::Quaterniond q_gt_estimate(gt_transformed.block<3, 3>(0, 0));

        nav_msgs::Odometry gt_pub;
        gt_pub.pose.pose.position.x = gt_transformed(0, 3);
        gt_pub.pose.pose.position.y = gt_transformed(1, 3);
        gt_pub.pose.pose.position.z = gt_transformed(2, 3);
        gt_pub.pose.pose.orientation.w = q_gt_estimate.w();
        gt_pub.pose.pose.orientation.x = q_gt_estimate.x();
        gt_pub.pose.pose.orientation.y = q_gt_estimate.y();
        gt_pub.pose.pose.orientation.z = q_gt_estimate.z();

        // publish mocap_msg_ptr aligned
        pubGt(gt_pub);
    }
}

void gt_pub_callback(const nav_msgs::Odometry::ConstPtr &ground_truth)
{
    if (first_receive_gt)
    {
        Eigen::Matrix4d estimate_to_gt;
        estimate_to_gt.setIdentity(4, 4);
        Eigen::Quaterniond q(ground_truth->pose.pose.orientation.w, ground_truth->pose.pose.orientation.x,
                             ground_truth->pose.pose.orientation.y, ground_truth->pose.pose.orientation.z);
        estimate_to_gt.block<3, 3>(0, 0) = q.toRotationMatrix();
        estimate_to_gt.block<3, 1>(0, 3) << ground_truth->pose.pose.position.x, ground_truth->pose.pose.position.y,
            ground_truth->pose.pose.position.z;
        gt_to_estimate = estimate_to_gt.inverse();

        Eigen::Matrix4d gt_transformed = gt_to_estimate * estimate_to_gt;

        std::cout << "estimate_to_gt: " << estimate_to_gt << std::endl;
        std::cout << "gt_to_estimate: " << gt_to_estimate << std::endl;
        std::cout << "gt_transformed: " << gt_transformed << std::endl;

        Eigen::Quaterniond q_gt_estimate(gt_transformed.block<3, 3>(0, 0));

        nav_msgs::Odometry gt_pub;
        gt_pub.pose.pose.position.x = gt_transformed(0, 3);
        gt_pub.pose.pose.position.y = gt_transformed(1, 3);
        gt_pub.pose.pose.position.z = gt_transformed(2, 3);
        gt_pub.pose.pose.orientation.w = q_gt_estimate.w();
        gt_pub.pose.pose.orientation.x = q_gt_estimate.x();
        gt_pub.pose.pose.orientation.y = q_gt_estimate.y();
        gt_pub.pose.pose.orientation.z = q_gt_estimate.z();

        // publish ground_truth aligned
        pubGt(gt_pub);

        first_receive_gt = false;
    }
    else
    {
        Eigen::Matrix4d estimate_to_gt;
        estimate_to_gt.setIdentity(4, 4);
        Eigen::Quaterniond q(ground_truth->pose.pose.orientation.w, ground_truth->pose.pose.orientation.x,
                             ground_truth->pose.pose.orientation.y, ground_truth->pose.pose.orientation.z);
        estimate_to_gt.block<3, 3>(0, 0) = q.toRotationMatrix();
        estimate_to_gt.block<3, 1>(0, 3) << ground_truth->pose.pose.position.x, ground_truth->pose.pose.position.y,
            ground_truth->pose.pose.position.z;

        Eigen::Matrix4d gt_transformed = gt_to_estimate * estimate_to_gt;

        Eigen::Quaterniond q_gt_estimate(gt_transformed.block<3, 3>(0, 0));

        nav_msgs::Odometry gt_pub;
        gt_pub.pose.pose.position.x = gt_transformed(0, 3);
        gt_pub.pose.pose.position.y = gt_transformed(1, 3);
        gt_pub.pose.pose.position.z = gt_transformed(2, 3);
        gt_pub.pose.pose.orientation.w = q_gt_estimate.w();
        gt_pub.pose.pose.orientation.x = q_gt_estimate.x();
        gt_pub.pose.pose.orientation.y = q_gt_estimate.y();
        gt_pub.pose.pose.orientation.z = q_gt_estimate.z();

        // publish ground_truth aligned
        pubGt(gt_pub);
    }
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "vins_estimator");
    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);

    if (argc != 2)
    {
        printf("please intput: rosrun vins vins_node [config file] \n"
               "for example: rosrun vins vins_node "
               "~/catkin_ws/src/VINS-Fusion/config/euroc/euroc_stereo_imu_config.yaml \n");
        return 1;
    }

    string config_file = argv[1];
    printf("config_file: %s\n", argv[1]);

    readParameters(config_file);
    estimator.setParameter();

#ifdef EIGEN_DONT_PARALLELIZE
    ROS_DEBUG("EIGEN_DONT_PARALLELIZE");
#endif

    ROS_WARN("waiting for image and imu...");

    registerPub(n);

    ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 2000, imu_callback, ros::TransportHints().tcpNoDelay());
    ros::Subscriber sub_feature = n.subscribe("/feature_tracker/feature", 2000, feature_callback);
    ros::Subscriber sub_img0 = n.subscribe(IMAGE0_TOPIC, 100, img0_callback);
    ros::Subscriber sub_img1 = n.subscribe(IMAGE1_TOPIC, 100, img1_callback);

    ros::Subscriber sub_mocap = n.subscribe("/mavros/vision_pose/pose", 100, mocap_callback);

    // ros::Subscriber sub_ground_truth = n.subscribe("/mavros/local_position/pose", 100, gt_pub_callback);

    std::thread sync_thread{sync_process};
    ros::spin();

    return 0;
}
