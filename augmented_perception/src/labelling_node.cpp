#include <ros/package.h>
#include "laser_geometry/laser_geometry.h"
#include "message_filters/subscriber.h"
#include "ros/ros.h"
#include "sensor_msgs/PointCloud.h"
#include "tf/message_filter.h"
#include "tf/transform_listener.h"

#include "mtt/TargetList.h"
#include "mtt/mtt.h"

#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>

#include <opencv2/opencv.hpp>

#include <sys/stat.h>
#include <ctime>
#include <iostream>
#include <string>

#include "common.cpp"

using namespace std;
using namespace cv;

// Publishers
ros::Publisher pub_scan_0;
ros::Publisher pub_scan_0_filtered;
image_transport::Publisher pub_image;
ros::Publisher markers_publisher;
ros::Publisher marker_publisher;

// Images
cv_bridge::CvImagePtr cv_ptr;
Mat image_input, imToShow, sub;

// Template-Matching related variables
Mat patch, first_patch, result;
Point2f pointdown, pointup, pointbox;

/* 0: Squared Difference
 * 1: Normalized Squared Difference
 * 2: Cross Correlation
 * 3: Normalized Cross Correlation
 * 4: Cross Correlation Coefficient
 * 5: Normalized Cross Correlation Coefficient
 */
int match_method = CV_TM_SQDIFF;  // 0: CV_TM_SQDIFF 1: CV_TM_SQDIFF_NORMED 2: CV_TM_CCORR
                                  // 3: CV_TM_CCORR_NORMED 4: CV_TM_CCOEFF 5: CV_TM_CCOEFF_NORMED
int max_Trackbar = 5;
int nframes = 0;
bool capture = false;
bool drawRect = false;
bool got_last_patches = false;
std::queue<Mat> frame_array;
std::queue<Mat> previous_frames;
std::queue<Mat> first_previous_frames;
std::queue<Mat> previous_patches;

// Scanner MTT related variables
sensor_msgs::PointCloud2 pointData;

mtt::TargetListPC targetList;

t_config config;
t_data full_data;
t_flag flags;

vector<t_clustersPtr> clusters;
vector<t_objectPtr> object;
vector<t_listPtr> list_vector;

visualization_msgs::MarkerArray markersMsg;

// File writing related variables

struct BBox
{
  int x;
  int y;
  int width;
  int height;
};

std::map<unsigned int, std::vector<BBox> > file_map;

Mat MatchingMethod(int, void *, Mat patch_frame, Mat previous_frame)
{
  Mat img_display;
  previous_frame.copyTo(img_display);
  int result_cols = previous_frame.cols - patch_frame.cols + 1;
  int result_rows = previous_frame.rows - patch_frame.rows + 1;
  result.create(result_rows, result_cols, CV_32FC1);

  matchTemplate(previous_frame, patch_frame, result, match_method);

  normalize(result, result, 0, 1, NORM_MINMAX, -1, Mat());

  double minVal;
  double maxVal;
  Point minLoc;
  Point maxLoc;
  Point matchLoc;

  minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc, Mat());

  if (match_method == TM_SQDIFF || match_method == TM_SQDIFF_NORMED)
  {
    matchLoc = minLoc;
  }
  else
  {
    matchLoc = maxLoc;
  }

  cv::Rect myROI(matchLoc.x, matchLoc.y, patch_frame.cols, patch_frame.rows);

  patch_frame = img_display(myROI);

  return patch_frame;
}

void MatchingMethod(int, void *)
{
  Mat img_display;
  sub.copyTo(img_display);
  int result_cols = sub.cols - patch.cols + 1;
  int result_rows = sub.rows - patch.rows + 1;
  result.create(result_rows, result_cols, CV_32FC1);

  matchTemplate(sub, patch, result, match_method);

  normalize(result, result, 0, 1, NORM_MINMAX, -1, Mat());

  double minVal;
  double maxVal;
  Point minLoc;
  Point maxLoc;
  Point matchLoc;

  minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc, Mat());

  if (match_method == TM_SQDIFF || match_method == TM_SQDIFF_NORMED)
  {
    matchLoc = minLoc;
  }
  else
  {
    matchLoc = maxLoc;
  }

  // update patch
  cv::Rect myROI(matchLoc.x, matchLoc.y, patch.cols, patch.rows);
  patch = img_display(myROI);

  unsigned int frame_seq = cv_ptr->header.seq;

  BBox box;
  box.x = matchLoc.x;
  box.y = matchLoc.y;
  box.width = patch.cols;
  box.height = patch.rows;

  file_map[frame_seq].push_back(box);

  // limit 100
  if (frame_array.size() >= 100)
  {
    frame_array.pop();
  }

  frame_array.push(patch.clone());

  nframes++;

  rectangle(imToShow, matchLoc, Point(matchLoc.x + patch.cols, matchLoc.y + patch.rows), Scalar(0, 0, 255), 2, 8, 0);
  rectangle(result, matchLoc, Point(matchLoc.x + patch.cols, matchLoc.y + patch.rows), Scalar::all(0), 2, 8, 0);
  imshow("camera", imToShow);
  return;
}

static void onMouse_TM(int event, int x, int y, int /*flags*/, void * /*param*/)
{
  if (event == EVENT_LBUTTONDOWN)
  {
    pointdown = Point2f((float)x, (float)y);
    drawRect = true;
  }
  if (event == EVENT_LBUTTONUP)
  {
    pointup = Point2f((float)x, (float)y);

    if (x < 0)
      pointup.x = 0;

    if (y < 0)
      pointup.y = 0;

    if (x > image_input.cols)
      pointup.x = image_input.cols;

    if (y > image_input.rows)
      pointup.y = image_input.rows;

    capture = true;
    got_last_patches = false;
    nframes = 0;

    while (frame_array.size() > 0)
    {
      frame_array.pop();
    }
  }

  pointbox = Point2f((float)x, (float)y);
}

void image_cb_TemplateMatching(const sensor_msgs::ImageConstPtr &msg)
{
  try
  {
    cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);

    char c = (char)waitKey(10);

    if (c == 'q')
    {
      exit(0);
    }

    if (c == 'p')
    {
      string path = ros::package::getPath("augmented_perception") + "/datasets/";
      mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

      ofstream myfile;
      string filename = path + boost::lexical_cast<std::string>(std::time(NULL)) + ".txt";
      myfile.open(filename.c_str());
      myfile << "FRAME_ID\nBOX_X BOX_Y WIDTH HEIGHT\n";

      for (std::map<unsigned int, std::vector<BBox> >::iterator it = file_map.begin(); it != file_map.end(); ++it)
      {
        myfile << it->first << endl;
        for (int i = 0; i < (it->second).size(); i++)
        {
          myfile << it->second[i].x << " " << it->second[i].y << " " << it->second[i].width << " "
                 << it->second[i].height << endl;
        }
      }
      myfile.close();
      ROS_INFO("Saved frames dataset to %s", filename.c_str());
    }

    if (c == 's')
    {
      int gotframes = nframes;
      if (gotframes > 100)
        gotframes = 100;

      nframes = 0;

      if (gotframes == 0 || patch.empty())
      {
        ROS_INFO("There are no frames to save.");
      }
      else
      {
        ROS_INFO("Loading templates to save...");

        // get 5 last patches
        if (!got_last_patches)
        {
          Mat previous_patch = first_patch;
          for (int i = 0; i < 5; i++)
          {
            previous_patch = MatchingMethod(0, 0, previous_patch, first_previous_frames.front());
            first_previous_frames.pop();
            previous_patches.push(previous_patch);
          }
          got_last_patches = true;
        }

        ROS_INFO("Saving %s frames. Please enter object label: (type 'exit' to discard frames)",
                 boost::lexical_cast<std::string>(gotframes + 5).c_str());

        string path;
        path = ros::package::getPath("augmented_perception") + "/labelling/";
        mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

        cin >> path;

        if (path.find("exit"))
        {
          path = ros::package::getPath("augmented_perception") + "/labelling/" + path;
          mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

          path += "/" + boost::lexical_cast<std::string>(std::time(NULL));
          mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

          for (int i = 0; i < gotframes; i++)
          {
            string impath;
            impath = path + "/" + boost::lexical_cast<std::string>(i + 1) + ".bmp";
            imwrite(impath, frame_array.front());
            frame_array.pop();
          }

          int i = 0;
          while (!previous_patches.empty())
          {
            i++;
            string impath;
            impath = path + "/previous_" + boost::lexical_cast<std::string>(i) + ".bmp";
            imwrite(impath, previous_patches.front());
            previous_patches.pop();
            previous_frames.pop();
          }

          ROS_INFO("Saved %s frames to %s", boost::lexical_cast<std::string>(gotframes + 5).c_str(), path.c_str());
        }
        else
        {
          cout << "Did not save\n";
        }
      }
    }
    if (c == 'c')
    {
      patch = Mat();
      ROS_INFO("Image cleared");
    }

    // Show image_input
    image_input = cv_ptr->image;
    imToShow = image_input.clone();
    sub = image_input.clone();

    // previous 5 frames
    if (previous_frames.size() >= 5)
    {
      previous_frames.pop();
    }

    previous_frames.push(image_input);

    if (drawRect)
    {
      // Draw area-to-crop rectangle
      float max_x, min_x, max_y, min_y;
      if (pointbox.x > pointdown.x)
      {
        min_x = pointdown.x;
        max_x = pointbox.x;
      }
      else
      {
        max_x = pointdown.x;
        min_x = pointbox.x;
      }
      if (pointbox.y > pointdown.y)
      {
        min_y = pointdown.y;
        max_y = pointbox.y;
      }
      else
      {
        max_y = pointdown.y;
        min_y = pointbox.y;
      }

      cv::rectangle(imToShow, Point((int)min_x, (int)min_y), Point((int)max_x, (int)max_y), Scalar(0, 255, 0), 3);
    }

    cv::imshow("camera", imToShow);
    cvSetMouseCallback("camera", onMouse_TM, 0);
  }
  catch (cv_bridge::Exception &e)
  {
    ROS_ERROR("Could not convert from '%s' to 'bgr8'.", msg->encoding.c_str());
  }

  float max_x, min_x, max_y, min_y;
  if (pointup.x > pointdown.x)
  {
    min_x = pointdown.x;
    max_x = pointup.x;
  }
  else
  {
    max_x = pointdown.x;
    min_x = pointup.x;
  }
  if (pointup.y > pointdown.y)
  {
    min_y = pointdown.y;
    max_y = pointup.y;
  }
  else
  {
    max_y = pointdown.y;
    min_y = pointup.y;
  }

  // get first patch and previous frames
  if (max_x - min_x > 0 && max_y - min_y > 0 && capture)
  {
    cv::Rect myROI(min_x, min_y, max_x - min_x, max_y - min_y);
    patch = image_input(myROI);
    first_patch = patch.clone();
    while (!previous_frames.empty())
    {
      first_previous_frames.push(previous_frames.front());
      previous_frames.pop();
    }

    capture = false;
    drawRect = false;
  }

  // up half ignore
  for (int y = 0; y < sub.rows / 3; y++)
  {
    for (int x = 0; x < sub.cols; x++)
    {
      sub.at<Vec3b>(Point(x, y))[0] = 0;
      sub.at<Vec3b>(Point(x, y))[1] = 0;
      sub.at<Vec3b>(Point(x, y))[2] = 0;
    }
  }

  if (!patch.empty())
  {
    imshow("crop", patch);
    MatchingMethod(0, 0);
  }
}

void scan_0_cb(const sensor_msgs::LaserScan::ConstPtr &msg)
{
  laser_geometry::LaserProjection projector;

  projector.projectLaser(*msg, pointData);

  // Get data from PointCloud2 to full_data
  PointCloud2ToData(pointData, full_data);

  // clustering
  clustering(full_data, clusters, &config, &flags);

  // calc_cluster_props
  calc_cluster_props(clusters, full_data);

  // clusters2objects
  clusters2objects(object, clusters, full_data, config);

  calc_object_props(object);

  // AssociateObjects
  AssociateObjects(list_vector, object, config, flags);

  // MotionModelsIteration
  MotionModelsIteration(list_vector, config);

  // cout<<"Number of targets "<<list_vector.size() << endl;

  free_lines(object);  // clean current objects

  targetList.id.clear();
  targetList.obstacle_lines.clear();  // clear all lines

  pcl::PointCloud<pcl::PointXYZ> target_positions;
  pcl::PointCloud<pcl::PointXYZ> velocity;

  target_positions.header.frame_id = pointData.header.frame_id;

  velocity.header.frame_id = pointData.header.frame_id;

  targetList.header.stamp = ros::Time::now();
  targetList.header.frame_id = pointData.header.frame_id;

  // cout << "list size: " << list_vector.size() << endl;

  for (uint i = 0; i < list_vector.size(); i++)
  {
    targetList.id.push_back(list_vector[i]->id);

    pcl::PointXYZ position;

    position.x = list_vector[i]->position.estimated_x;
    position.y = list_vector[i]->position.estimated_y;
    position.z = 0;

    target_positions.points.push_back(position);

    pcl::PointXYZ vel;

    vel.x = list_vector[i]->velocity.velocity_x;
    vel.y = list_vector[i]->velocity.velocity_y;
    vel.z = 0;

    velocity.points.push_back(vel);

    pcl::PointCloud<pcl::PointXYZ> shape;
    pcl::PointXYZ line_point;

    uint j;
    for (j = 0; j < list_vector[i]->shape.lines.size(); j++)
    {
      line_point.x = list_vector[i]->shape.lines[j]->xi;
      line_point.y = list_vector[i]->shape.lines[j]->yi;

      shape.points.push_back(line_point);
    }

    line_point.x = list_vector[i]->shape.lines[j - 1]->xf;
    line_point.y = list_vector[i]->shape.lines[j - 1]->yf;

    sensor_msgs::PointCloud2 shape_cloud;
    pcl::toROSMsg(shape, shape_cloud);
    targetList.obstacle_lines.push_back(shape_cloud);
  }

  pcl::toROSMsg(target_positions, targetList.position);
  pcl::toROSMsg(velocity, targetList.velocity);

  pub_scan_0.publish(targetList);

  CreateMarkers(markersMsg.markers, targetList, list_vector);

  markers_publisher.publish(markersMsg);

  flags.fi = false;
}

void laserFilter(const sensor_msgs::LaserScan::ConstPtr &input)
{
  int n_pos = (input->angle_max - input->angle_min) / input->angle_increment;

  // Create a container for the data.
  sensor_msgs::LaserScan output;
  output = *input;

  // Do something with cloud.

  // left half
  float left_limit[100] = { 6.1,    6.1,   6.1,    6.1,    6.1,    6.1,    6.1,    6.1,    6.1,   6.1,    6.1,
                            6.2,    6.3,   6.385,  6.47,   6.505,  6.54,   6.625,  6.71,   6.765, 6.82,   6.905,
                            6.99,   7.065, 7.14,   7.225,  7.31,   7.39,   7.47,   7.57,   7.67,  7.79,   7.91,
                            7.99,   8.07,  8.19,   8.31,   8.45,   8.59,   8.705,  8.82,   8.965, 9.11,   9.245,
                            9.38,   9.545, 9.71,   9.885,  10.06,  10.225, 10.39,  10.585, 10.78, 10.965, 11.15,
                            11.405, 11.66, 11.905, 12.15,  12.425, 12.7,   13,     13.3,   13.6,  13.9,   14.22,
                            14.5,   14.9,  15.38,  15.8,   16.22,  16.7,   17.18,  17.76,  18.34, 18.94,  19.54,
                            20.26,  20.98, 21.8,   22.62,  23.58,  24.54,  25.68,  26.82,  28.18, 29.54,  31.135,
                            32.73,  34.81, 36.89,  40.215, 43.54,  91.91,  140.28, 170.14, 200,   200,    200 };

  for (int i = 0; i < n_pos / 2; i++)
  {
    if (output.ranges[i] > left_limit[i])
    {
      output.ranges[i] = 0.0;
    }
  }

  // right half
  float right_limit[100] = { 200,  200,  200,  200,  85,   73,    60,   53,    48,   40,   35,   30,   28,   26,   25,
                             23.5, 22,   20,   18,   17,   16,    15,   14.5,  14,   13.5, 13,   12.8, 12.6, 12.3, 12,
                             11.7, 11.4, 11.1, 10.8, 10.5, 10.2,  9.9,  9.6,   9.3,  9,    8.7,  8.4,  8.1,  7.9,  7.75,
                             7.6,  7.5,  7.35, 7.25, 7.15, 7.05,  6.95, 6.8,   6.7,  6.6,  6.5,  6.4,  6.3,  6.2,  6.1,
                             6,    5.9,  5.8,  5.7,  5.6,  5.5,   5.4,  5.345, 5.3,  5.2,  5.1,  5.15, 5.1,  5,    4.95,
                             4.9,  4.85, 4.8,  4.7,  4.68, 4.6,   4.57, 4.53,  4.5,  4.47, 4.43, 4.4,  4.35, 4.3,  4.25,
                             4.20, 4.18, 4.15, 4.12, 4.08, 4.025, 3.98, 3.96,  3.93, 3.89 };

  for (int i = 0; i < n_pos / 2; i++)
  {
    // output.ranges[i + n_pos / 2] = right_limit[i];

    if (output.ranges[i + n_pos / 2] > right_limit[i])
    {
      output.ranges[i + n_pos / 2] = 0.0;
    }
  }

  // Publish the data.
  pub_scan_0_filtered.publish(output);
}

int main(int argc, char **argv)
{
  // Initialize ROS
  ros::init(argc, argv, "labelling_node");
  ros::NodeHandle nh;
  image_transport::ImageTransport it(nh);

  // Create Camera Windows
  cv::namedWindow("camera", CV_WINDOW_NORMAL);
  cv::startWindowThread();

  cv::namedWindow("crop", CV_WINDOW_NORMAL);
  cv::startWindowThread();

  // Create a ROS subscriber for the inputs
  ros::Subscriber sub_scan_0 = nh.subscribe("/ld_rms/scan0", 1, laserFilter);
  ros::Subscriber sub_scan_0_mtt = nh.subscribe("/filter/scan0", 1, scan_0_cb);
  image_transport::Subscriber sub_image = it.subscribe("/camera/image_color", 1, image_cb_TemplateMatching);

  // Create a ROS publisher for the output point cloud
  pub_image = it.advertise("output/image_input", 1);
  pub_scan_0_filtered = nh.advertise<sensor_msgs::LaserScan>("/filter/scan0", 1000);
  pub_scan_0 = nh.advertise<mtt::TargetListPC>("/targets", 1000);
  markers_publisher = nh.advertise<visualization_msgs::MarkerArray>("/markers", 1000);
  marker_publisher = nh.advertise<visualization_msgs::Marker>("/marker", 1000);

  init_flags(&flags);    // Inits flags values
  init_config(&config);  // Inits configuration values

  cout << "Keyboard Controls:\n";
  cout << "[Q]uit\n[C]lear image\n[S]ave templates\n[P]rint File\n";

  // Spin
  ros::spin();
  cv::destroyAllWindows();
}
