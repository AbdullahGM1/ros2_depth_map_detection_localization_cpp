// Code with just Sync


#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/point_cloud2.hpp"
#include "sensor_msgs/msg/image.hpp"
#include "geometry_msgs/msg/pose_array.hpp"
#include "pcl/point_cloud.h"
#include "pcl_conversions/pcl_conversions.h"
#include "pcl/point_types.h"
#include "pcl/filters/passthrough.h"
#include "cv_bridge/cv_bridge.h"
#include "opencv2/opencv.hpp"
#include "yolov8_msgs/msg/detection_array.hpp"
#include <vector>
#include <stdexcept>
#include "pcl/filters/extract_indices.h"
#include "message_filters/subscriber.h"
#include "message_filters/sync_policies/approximate_time.h"
#include "message_filters/time_synchronizer.h"

class PointCloudToDepthMap : public rclcpp::Node
{
public:
    PointCloudToDepthMap()
        : Node("point_cloud_to_depth_map")
    {
        // Declare parameters with default values
        this->declare_parameter<int>("width", 650);
        this->declare_parameter<int>("height", 650);
        this->declare_parameter<float>("scale", 50.0);
        this->declare_parameter<float>("MinDepth", 0.2f);
        this->declare_parameter<float>("MaxDepth", 30.0f);

         // Fetch parameters
        this->get_parameter("width", width_);
        this->get_parameter("height", height_);
        this->get_parameter("scale", scale_);
        this->get_parameter("MinDepth", MinDepth_);
        this->get_parameter("MaxDepth", MaxDepth_);

        // Log parameters
        RCLCPP_INFO(this->get_logger(), "Loaded Parameters: width=%d, height=%d, scale=%f, MinDepth=%f, MaxDepth=%f",
                    width_, height_, scale_, MinDepth_, MaxDepth_);
        
    
        // Subscriber for PointCloud2 messages
        subscription_ = this->create_subscription<sensor_msgs::msg::PointCloud2>(
            "/scan/points", 10, std::bind(&PointCloudToDepthMap::point_cloud_callback, this, std::placeholders::_1));

        // Create subscribers using message_filters
        pointcloud_sub_.subscribe(this, "/scan/points");
        detection_sub_.subscribe(this, "/depth_map/tracking");

        // Create synchronization policy
        sync_ = std::make_shared<message_filters::Synchronizer<SyncPolicy>>(
            SyncPolicy(10), pointcloud_sub_, detection_sub_);
        
        // Register callback
        sync_->registerCallback(std::bind(&PointCloudToDepthMap::sync_callback, this, std::placeholders::_1, std::placeholders::_2));
        
        // Publisher for original depth map
        original_publisher_ = this->create_publisher<sensor_msgs::msg::Image>("/depth_map", 10);

        // Publisher for detected object depth map
        detected_object_publisher_ = this->create_publisher<sensor_msgs::msg::Image>("/detected_object_depth_map", 10);
        
        // Publisher for detected object poses
        detected_object_pose_publisher_ = this->create_publisher<geometry_msgs::msg::PoseArray>("/detected_object_depthmap_pose", 10);
                
        RCLCPP_INFO(this->get_logger(), "PointCloud to Depth Map Node has been started.");
    }

private:

    // method to handle filtering
    pcl::PointCloud<pcl::PointXYZ>::Ptr filterPointCloud(const pcl::PointCloud<pcl::PointXYZ>::Ptr& input_cloud) 
    {
        pcl::PointCloud<pcl::PointXYZ>::Ptr filtered_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::PointCloud<pcl::PointXYZ>::Ptr temp_cloud(new pcl::PointCloud<pcl::PointXYZ>);

        // Z-axis filtering (depth)
        pcl::PassThrough<pcl::PointXYZ> pass_z;
        pass_z.setInputCloud(input_cloud);
        pass_z.setFilterFieldName("z");
        pass_z.setFilterLimits(MinDepth_, MaxDepth_);
        pass_z.filter(*temp_cloud);

        // X-axis filtering - Negative and Positive ranges
        pcl::PassThrough<pcl::PointXYZ> pass_x;
        pass_x.setInputCloud(temp_cloud);
        pass_x.setFilterFieldName("x");
        
        pcl::PointCloud<pcl::PointXYZ>::Ptr x_neg_filtered(new pcl::PointCloud<pcl::PointXYZ>);
        pass_x.setFilterLimits(-MaxDepth_, -MinDepth_);
        pass_x.setNegative(false);
        pass_x.filter(*x_neg_filtered);

        pcl::PointCloud<pcl::PointXYZ>::Ptr x_pos_filtered(new pcl::PointCloud<pcl::PointXYZ>);
        pass_x.setFilterLimits(MinDepth_, MaxDepth_);
        pass_x.setNegative(false);
        pass_x.filter(*x_pos_filtered);

        // Combine X filtered clouds
        pcl::PointCloud<pcl::PointXYZ>::Ptr x_combined_filtered(new pcl::PointCloud<pcl::PointXYZ>);
        *x_combined_filtered = *x_neg_filtered + *x_pos_filtered;

        // Y-axis filtering - Negative and Positive ranges
        pcl::PassThrough<pcl::PointXYZ> pass_y;
        pass_y.setInputCloud(x_combined_filtered);
        pass_y.setFilterFieldName("y");
        
        pcl::PointCloud<pcl::PointXYZ>::Ptr y_neg_filtered(new pcl::PointCloud<pcl::PointXYZ>);
        pass_y.setFilterLimits(-MaxDepth_, -MinDepth_);
        pass_y.setNegative(false);
        pass_y.filter(*y_neg_filtered);

        pcl::PointCloud<pcl::PointXYZ>::Ptr y_pos_filtered(new pcl::PointCloud<pcl::PointXYZ>);
        pass_y.setFilterLimits(MinDepth_, MaxDepth_);
        pass_y.setNegative(false);
        pass_y.filter(*y_pos_filtered);

        // Final filtered cloud combines both Y ranges
        *filtered_cloud = *y_neg_filtered + *y_pos_filtered;
        return filtered_cloud;
    }

    void point_cloud_callback(const sensor_msgs::msg::PointCloud2::SharedPtr msg)
    {
        // Convert ROS PointCloud2 to PCL PointCloud
        pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::fromROSMsg(*msg, *pcl_cloud);

        // Create empty single-channel depth maps
        cv::Mat original_depth_map_single = cv::Mat::zeros(height_, width_, CV_8UC1);
        cv::Mat detected_object_depth_map_single = cv::Mat::zeros(height_, width_, CV_8UC1);

        // Apply filtering to remove far-away points
        auto filtered_cloud = filterPointCloud(pcl_cloud);

        // Optional: Log the number of points before and after filtering
        // RCLCPP_INFO(this->get_logger(), "Original point cloud size: %zu", pcl_cloud->size());
        // RCLCPP_INFO(this->get_logger(), "Filtered point cloud size: %zu", filtered_cloud->size());

        // Define the center of the depth map
        int center_x = width_ / 2;
        int center_y = height_ / 2;

        // Iterate through the filtered point cloud and map points to depth maps
        for (const auto& point : filtered_cloud->points)
        {
            float x = point.x;
            float y = point.y;
            float z = point.z;

            // Map x and y to pixel coordinates
            // int pixel_x = static_cast<int>(center_x + x * scale_);
            // int pixel_y = static_cast<int>(center_y - y * scale_);
            // Map x and y to pixel coordinates
            int pixel_x = center_x + static_cast<int>(ceil(y * scale_) * -1);
            int pixel_y = center_y + static_cast<int>(ceil(x * scale_) * -1);

            // Check if the pixel is within image bounds
            if (pixel_x >= 0 && pixel_x < width_ && pixel_y >= 0 && pixel_y < height_)
            {
                // Normalize depth value (x) to 0-255
                int depth_value = std::clamp(static_cast<int>(z * 255 / MaxDepth_), 0, 255);
                
                // Original depth map for all points
                original_depth_map_single.at<uint8_t>(pixel_y, pixel_x) = 255 - depth_value;

             }
        }

        // Convert the single-channel depth maps to 3-channel images
        cv::Mat original_depth_map, detected_object_depth_map;
        cv::cvtColor(original_depth_map_single, original_depth_map, cv::COLOR_GRAY2BGR);

        // Convert the depth maps to ROS Image messages
        sensor_msgs::msg::Image::SharedPtr original_image_msg = 
            cv_bridge::CvImage(std_msgs::msg::Header(), "bgr8", original_depth_map).toImageMsg();

        original_image_msg->header = msg->header;

        // Publish depth maps and poses
        original_publisher_->publish(*original_image_msg);
    }

    // Define the callback
    void sync_callback(const sensor_msgs::msg::PointCloud2::ConstSharedPtr pointcloud_msg,
                  const yolov8_msgs::msg::DetectionArray::ConstSharedPtr detection_msg)
    {
          // Define the center of the depth map
        int center_x = width_ / 2;
        int center_y = height_ / 2;
        
        // RCLCPP_INFO(this->get_logger(), "Received synchronized messages!");

        geometry_msgs::msg::PoseArray detected_object_poses;
        detected_object_poses.header = pointcloud_msg->header;

        // Convert ROS PointCloud2 to PCL PointCloud
        pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::fromROSMsg(*pointcloud_msg, *pcl_cloud);

        // Use the common filtering method
        auto filtered_cloud = filterPointCloud(pcl_cloud);

        //Create the detected object depth map
        cv::Mat detected_object_depth_map_single = cv::Mat::zeros(height_, width_, CV_8UC1);

        // Log the number of points before and after filtering
        // RCLCPP_INFO(this->get_logger(), "Original point cloud size: %zu", pcl_cloud->size());
        // RCLCPP_INFO(this->get_logger(), "Filtered point cloud size: %zu", filtered_cloud->size());

        // Loop through each BB
        for (const auto& bbox : detection_msg->detections)
        {
            // Extract the bounding box's center and size
            double x_center = bbox.bbox.center.position.x;
            double y_center = bbox.bbox.center.position.y;
            double width = bbox.bbox.size.x;
            double height = bbox.bbox.size.y;

            // Calculate the bounding box edges
            double x_min = x_center - width / 2.0;
            double x_max = x_center + width / 2.0;
            double y_min = y_center - height / 2.0;
            double y_max = y_center + height / 2.0;

            // RCLCPP_INFO(this->get_logger(), "BoundingBox center: x=%f, y=%f",
            //     x_center, y_center);

            // Declare variables before the loop
            double sum_x = 0.0;  
            double sum_y = 0.0;  
            double sum_z = 0.0;  
            int point_count = 0; 

            for (const auto& point : filtered_cloud->points)
            {
                int pixel_x = center_x + static_cast<int>(ceil(point.y * scale_) * -1);
                int pixel_y = center_y + static_cast<int>(ceil(point.x * scale_) * -1);

                // Normalize depth value (x) to 0-255
                int depth_value = std::clamp(static_cast<int>(point.z * 255 / MaxDepth_), 0, 255);

                // RCLCPP_INFO(this->get_logger(), "x_pixel=%d , y_pixel=%d", pixel_x, pixel_y);

                if (pixel_x >= x_min && pixel_x <= x_max &&
                        pixel_y >= y_min && pixel_y <= y_max)
                {
                        //Now these points are within the BB
                    
                        // RCLCPP_INFO(this->get_logger(), "x_pixel=%d , y_pixel=%d", pixel_x, pixel_y); 
                        //RCLCPP_INFO(this->get_logger(), "x =%f , y =%f ,z=%f", point.x, point.y, point.z); 

                        // Depth map for points within bounding boxes
                        detected_object_depth_map_single.at<uint8_t>(pixel_y, pixel_x) = 255 - depth_value;
                        
                        // Accumulate point coordinates
                        sum_x += point.x;
                        sum_y += point.y;
                        sum_z += point.z;
                        point_count++;
                }
            }  

             // Calculate average if points were found
            if (point_count > 0)
            {
                geometry_msgs::msg::Pose object_pose;
                object_pose.position.x = sum_x / point_count;
                object_pose.position.y = sum_y / point_count;
                object_pose.position.z = sum_z / point_count;

                // Optional: Set orientation to identity quaternion
                object_pose.orientation.x = 0.0;
                object_pose.orientation.y = 0.0;
                object_pose.orientation.z = 0.0;
                object_pose.orientation.w = 1.0;

                detected_object_poses.poses.push_back(object_pose);
                    
            }            
        }

        detected_object_pose_publisher_->publish(detected_object_poses);
        // Convert single-channel depth map to 3-channel image
        cv::Mat detected_object_depth_map;
        cv::cvtColor(detected_object_depth_map_single, detected_object_depth_map, cv::COLOR_GRAY2BGR);

        // Convert the depth map to a ROS image message
        sensor_msgs::msg::Image::SharedPtr detected_object_image_msg = 
            cv_bridge::CvImage(std_msgs::msg::Header(), "bgr8", detected_object_depth_map).toImageMsg();

        // Set the header to match the point cloud message
        detected_object_image_msg->header = pointcloud_msg->header;

        // Publish the detected object depth map
        detected_object_publisher_->publish(*detected_object_image_msg);

    }

    rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr subscription_;
    rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr original_publisher_;
    rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr detected_object_publisher_;
    rclcpp::Publisher<geometry_msgs::msg::PoseArray>::SharedPtr detected_object_pose_publisher_;
    

    // Message filter subscribers
    message_filters::Subscriber<sensor_msgs::msg::PointCloud2> pointcloud_sub_;
    message_filters::Subscriber<yolov8_msgs::msg::DetectionArray> detection_sub_;

    // Synchronization policy
    typedef message_filters::sync_policies::ApproximateTime<
        sensor_msgs::msg::PointCloud2,
        yolov8_msgs::msg::DetectionArray>
        SyncPolicy;
    std::shared_ptr<message_filters::Synchronizer<SyncPolicy>> sync_;

    int width_;
    int height_;
    float scale_;
    float MinDepth_;
    float MaxDepth_;
};

int main(int argc, char *argv[])
{
    rclcpp::init(argc, argv);

    auto node = std::make_shared<PointCloudToDepthMap>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}