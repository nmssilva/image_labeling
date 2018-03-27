#include <ros/package.h>
#include "laser_geometry/laser_geometry.h"
#include "message_filters/subscriber.h"
#include "ros/ros.h"
#include "sensor_msgs/PointCloud.h"
#include "tf/message_filter.h"
#include "tf/transform_listener.h"

#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>

#include <opencv2/opencv.hpp>

#include <sys/stat.h>
#include <ctime>
#include <iostream>
#include <string>

using namespace std;
using namespace cv;

// Publishers
ros::Publisher pub_scan_E;
image_transport::Publisher pub_image;

// Images
cv_bridge::CvImagePtr cv_ptr;
Mat image_input, imToShow, sub;

// Optical-flow related variables
Mat gray, prevGray, frame;
TermCriteria termcrit(TermCriteria::COUNT | TermCriteria::EPS, 20, 0.03);
vector<Point2f> points[2];
Point2f point;
bool needToInit = false;
bool addRemovePt = false;
const int MAX_COUNT = 500;
Size subPixWinSize(10, 10), winSize(31, 31);

// Template-Matching related variables
Mat patch, result;
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
Mat frame_array[100];

void scan_E_cb(const sensor_msgs::LaserScan::ConstPtr& input)
{
  laser_geometry::LaserProjection projector;
  tf::TransformListener listener;

  int n_pos = (input->angle_max - input->angle_min) / input->angle_increment;

  // Create a container for the data.
  sensor_msgs::LaserScan output;
  output = *input;

  // Do something with cloud.

  int lb_front_pos = 275;
  int lb_rear_pos = 400;
  int rb_front_pos = 100;
  int rb_rear_pos = 60;

  float b_threshold = 2;
  float lb_front = output.ranges[lb_front_pos] - b_threshold;
  float lb_rear = output.ranges[lb_rear_pos] - b_threshold;
  float rb_front = output.ranges[rb_front_pos] - b_threshold;
  float rb_rear = output.ranges[rb_rear_pos] - b_threshold;

  float lb_increment = (lb_front - lb_rear) / (lb_rear_pos - lb_front_pos);
  float rb_increment = (rb_front - rb_rear) / (rb_rear_pos - rb_front_pos);

  for (int i = 0; i <= n_pos; i++)
  {
    if (i >= lb_front_pos && i <= lb_rear_pos)
    {
      float max_range = lb_rear + (lb_increment * (i - lb_front_pos));
      output.ranges[i] = max_range;
    }
    else
    {
      output.ranges[i] = 0;
    }
  }

  // Publish the data.
  pub_scan_E.publish(output);
}

static void onMouse_OF(int event, int x, int y, int /*flags*/, void* /*param*/)
{
  if (event == EVENT_LBUTTONDOWN)
  {
    point = Point2f((float)x, (float)y);
    addRemovePt = true;
  }
}

void image_cb_OpticalFlow(const sensor_msgs::ImageConstPtr& msg)
{
  try
  {
    cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);

    char c = (char)waitKey(10);

    switch (c)
    {
      case 'r':
        needToInit = true;
        break;
      case 'c':
        points[0].clear();
        points[1].clear();
        break;
    }

    // Show image_input
    image_input = cv_ptr->image;

    cvSetMouseCallback("camera", onMouse_OF, 0);
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("Could not convert from '%s' to 'bgr8'.", msg->encoding.c_str());
  }

  // up half ignore
  for (int y = 0; y < image_input.rows / 3; y++)
  {
    for (int x = 0; x < image_input.cols; x++)
    {
      image_input.at<Vec3b>(Point(x, y))[0] = 0;
      image_input.at<Vec3b>(Point(x, y))[1] = 0;
      image_input.at<Vec3b>(Point(x, y))[2] = 0;
    }
  }

  cvtColor(image_input, gray, COLOR_BGR2GRAY);

  if (needToInit)
  {
    // automatic initialization
    goodFeaturesToTrack(gray, points[1], MAX_COUNT, 0.01, 10, Mat(), 3, 3, 0, 0.04);
    cornerSubPix(gray, points[1], subPixWinSize, Size(-1, -1), termcrit);
    addRemovePt = false;
  }
  else if (!points[0].empty())
  {
    vector<uchar> status;
    vector<float> err;
    if (prevGray.empty())
      gray.copyTo(prevGray);

    calcOpticalFlowPyrLK(prevGray, gray, points[0], points[1], status, err, winSize, 3, termcrit, 0, 0.001);

    size_t i, k;
    for (i = k = 0; i < points[1].size(); i++)
    {
      if (addRemovePt)
      {
        if (norm(point - points[1][i]) <= 5)
        {
          addRemovePt = false;
          continue;
        }
      }

      if (!status[i])
        continue;

      points[1][k++] = points[1][i];
      circle(image_input, points[1][i], 10, Scalar(0, 255, 0), -1, 8);
    }
    points[1].resize(k);
  }
  if (addRemovePt && points[1].size() < (size_t)MAX_COUNT)
  {
    vector<Point2f> tmp;
    tmp.push_back(point);
    cornerSubPix(gray, tmp, winSize, Size(-1, -1), termcrit);
    points[1].push_back(tmp[0]);
    addRemovePt = false;
  }

  std::swap(points[1], points[0]);
  cv::swap(prevGray, gray);
  needToInit = false;

  // Publish data
  cv::imshow("camera", image_input);

  cv_bridge::CvImage out_msg;
  out_msg.header = msg->header;                           // Same timestamp and tf frame as input image_input
  out_msg.encoding = sensor_msgs::image_encodings::BGR8;  // Or whatever
  out_msg.image = image_input;                            // Your cv::Mat

  pub_image.publish(out_msg.toImageMsg());
}

static void onMouse_TM(int event, int x, int y, int /*flags*/, void* /*param*/)
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
  }

  pointbox = Point2f((float)x, (float)y);
}

void MatchingMethod(int, void*)
{
  Mat img_display;
  sub.copyTo(img_display);
  int result_cols = sub.cols - patch.cols + 1;
  int result_rows = sub.rows - patch.rows + 1;
  result.create(result_rows, result_cols, CV_32FC1);
  bool method_accepts_mask = (CV_TM_SQDIFF == match_method || match_method == CV_TM_CCORR_NORMED);

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

  cv::Rect myROI(matchLoc.x, matchLoc.y, patch.cols, patch.rows);
  patch = img_display(myROI);

  frame_array[nframes % 100] = patch.clone();

  nframes++;
  rectangle(imToShow, matchLoc, Point(matchLoc.x + patch.cols, matchLoc.y + patch.rows), Scalar(0, 0, 255), 2, 8, 0);
  rectangle(result, matchLoc, Point(matchLoc.x + patch.cols, matchLoc.y + patch.rows), Scalar::all(0), 2, 8, 0);
  imshow("camera", imToShow);
  return;
}

void image_cb_TemplateMatching(const sensor_msgs::ImageConstPtr& msg)
{
  try
  {
    cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);

    char c = (char)waitKey(10);

    if (c == 's')
    {
      int gotframes = nframes;
      if (gotframes > 100)
        gotframes = 100;

      nframes = 0;

      if (gotframes == 0)
      {
        ROS_INFO("Can't save 0 frames.");
      }
      else
      {
        ROS_INFO("Saving %d frames. Please enter object label: (type 'exit' to discard frames)", gotframes);

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
            imwrite(impath, frame_array[i]);
          }

          cout << "Saved " << gotframes << " frames to " << path << endl;
        }
        else
        {
          cout << "Exitting\n";
        }
      }
    }

    // Show image_input
    image_input = cv_ptr->image;
    imToShow = image_input.clone();
    sub = image_input.clone();

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
  catch (cv_bridge::Exception& e)
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

  if (max_x - min_x > 0 && max_y - min_y > 0 && capture)
  {
    cv::Rect myROI(min_x, min_y, max_x - min_x, max_y - min_y);
    patch = imToShow(myROI);
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

int main(int argc, char** argv)
{
  // Initialize ROS
  ros::init(argc, argv, "car_detection_node");
  ros::NodeHandle nh;
  image_transport::ImageTransport it(nh);

  // Create Camera Windows
  cv::namedWindow("camera", CV_WINDOW_NORMAL);
  cv::startWindowThread();

  cv::namedWindow("crop", CV_WINDOW_NORMAL);
  cv::startWindowThread();

  // Create a ROS subscriber for the inputs
  image_transport::Subscriber sub_image = it.subscribe("/camera/image_color", 1, image_cb_TemplateMatching);
  ros::Subscriber sub_scan_E = nh.subscribe("/lms151_E_scan", 1, scan_E_cb);

  // Create a ROS publisher for the output point cloud
  pub_image = it.advertise("output/image_input", 1);
  pub_scan_E = nh.advertise<sensor_msgs::LaserScan>("/output/scan_E", 1);

  // Spin
  ros::spin();
  cv::destroyAllWindows();
}