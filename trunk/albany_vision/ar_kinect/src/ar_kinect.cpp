/*
 *  Multi Marker Pose Estimation using ARToolkit & Kinect
 *  Copyright (C) 2010, CCNY Robotics Lab, 2011 ILS Robotics Lab
 *  Ivan Dryanovski <ivan.dryanovski@gmail.com>
 *  William Morris <morris@ee.ccny.cuny.edu>
 *  Gautier Dumonteil <gautier.dumonteil@gmail.com>
 *  Michael Ferguson <ferguson@cs.albany.edu>
 *  http://robotics.ccny.cuny.edu
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include "ar_kinect/ar_kinect.h"
#include "ar_kinect/object.h"

int main (int argc, char **argv)
{
  ros::init (argc, argv, "ar_kinect");
  ros::NodeHandle n;
  ar_pose::ARPublisher ar_kinect (n);
  ros::spin ();
  return 0;
}

namespace ar_pose
{
  ARPublisher::ARPublisher (ros::NodeHandle & n):n_ (n), it_ (n_), gotcloud_(false)
  {
    std::string path;
    std::string package_path = ros::package::getPath (ROS_PACKAGE_NAME);
    ros::NodeHandle n_param ("~");
    XmlRpc::XmlRpcValue xml_marker_center;
    cloud_width_ = 640;

    // **** get parameters

    if (!n_param.getParam ("publish_tf", publishTf_))
      publishTf_ = true;
    ROS_INFO ("\tPublish transforms: %d", publishTf_);

    if (!n_param.getParam ("publish_visual_markers", publishVisualMarkers_))
      publishVisualMarkers_ = true;
    ROS_INFO ("\tPublish visual markers: %d", publishVisualMarkers_);

    if (!n_param.getParam ("threshold", threshold_))
      threshold_ = 100;
    ROS_INFO ("\tThreshold: %d", threshold_);

    if (!n_param.getParam ("marker_pattern_list", path)){
      sprintf(pattern_filename_, "%s/data/objects_kinect", package_path.c_str());
    }else{
      sprintf(pattern_filename_, "%s", path.c_str());
    }    
    ROS_INFO ("Marker Pattern Filename: %s", pattern_filename_);

    if (!n_param.getParam ("marker_data_directory", path)){
      sprintf(data_directory_, "%s", package_path.c_str());
    }else{
      sprintf(data_directory_, "%s", path.c_str());
    }    
    ROS_INFO ("Marker Data Directory: %s", data_directory_);

    // **** subscribe

    ROS_INFO ("Subscribing to info topic");
    sub_ = n_.subscribe (cameraInfoTopic_, 1, &ARPublisher::camInfoCallback, this);
    getCamInfo_ = false;

    // **** advertsie 

    arMarkerPub_ = n_.advertise < ar_pose::ARMarkers > ("ar_pose_markers",0);
    if(publishVisualMarkers_)
    {
		rvizMarkerPub_ = n_.advertise < visualization_msgs::Marker > ("visualization_marker", 0);
	 }
  }

  ARPublisher::~ARPublisher (void)
  {
    arVideoCapStop ();
    arVideoClose ();
  }

  void ARPublisher::camInfoCallback (const sensor_msgs::CameraInfoConstPtr & cam_info)
  {
    if (!getCamInfo_)
    {
      cam_info_ = (*cam_info);

      cam_param_.xsize = cam_info_.width;
      cam_param_.ysize = cam_info_.height;

      cam_param_.mat[0][0] = cam_info_.P[0];
      cam_param_.mat[1][0] = cam_info_.P[4];
      cam_param_.mat[2][0] = cam_info_.P[8];
      cam_param_.mat[0][1] = cam_info_.P[1];
      cam_param_.mat[1][1] = cam_info_.P[5];
      cam_param_.mat[2][1] = cam_info_.P[9];
      cam_param_.mat[0][2] = cam_info_.P[2];
      cam_param_.mat[1][2] = cam_info_.P[6];
      cam_param_.mat[2][2] = cam_info_.P[10];
      cam_param_.mat[0][3] = cam_info_.P[3];
      cam_param_.mat[1][3] = cam_info_.P[7];
      cam_param_.mat[2][3] = cam_info_.P[11];

      cam_param_.dist_factor[0] = cam_info_.K[3];       // x0 = cX from openCV calibration
      cam_param_.dist_factor[1] = cam_info_.K[6];       // y0 = cY from openCV calibration
      cam_param_.dist_factor[2] = -100*cam_info_.D[0];  // f = -100*k1 from CV. Note, we had to do mm^2 to m^2, hence 10^8->10^2
      cam_param_.dist_factor[3] = 1.0;                  // scale factor, should probably be >1, but who cares...
      
      arInit ();

      ROS_INFO ("Subscribing to image and cloud topics");
      cam_sub_ = it_.subscribe (cameraImageTopic_, 1, &ARPublisher::getTransformationCallback, this);
      cloud_sub_ = n_.subscribe(cloudTopic_, 1, &ARPublisher::cloudCallback, this);

      getCamInfo_ = true;
    }
  }

  void ARPublisher::arInit ()
  {
    arInitCparam (&cam_param_);
    ROS_INFO ("*** Camera Parameter ***");
    arParamDisp (&cam_param_);

    // load in the object data - trained markers and associated bitmap files
    if ((object = ar_object::read_ObjData (pattern_filename_, data_directory_, &objectnum)) == NULL)
      ROS_BREAK ();
    ROS_DEBUG ("Objectfile num = %d", objectnum);

    sz_ = cvSize (cam_param_.xsize, cam_param_.ysize);
    capture_ = cvCreateImage (sz_, IPL_DEPTH_8U, 4);
  }

  void ARPublisher::getTransformationCallback (const sensor_msgs::ImageConstPtr & image_msg)
  {
    ARUint8 *dataPtr;
    ARMarkerInfo *marker_info;
    int marker_num;
    int i, k, j;

    /* Get the image from ROSTOPIC
     * NOTE: the dataPtr format is BGR because the ARToolKit library was
     * build with V4L, dataPtr format change according to the 
     * ARToolKit configure option (see config.h).*/
    try
    {
      capture_ = bridge_.imgMsgToCv (image_msg, "bgr8");
    }
    catch (sensor_msgs::CvBridgeException & e)
    {
      ROS_ERROR ("Could not convert from '%s' to 'bgr8'.", image_msg->encoding.c_str ());
    }
    //cvConvertImage(capture,capture,CV_CVTIMG_FLIP);
    dataPtr = (ARUint8 *) capture_->imageData;

    // detect the markers in the video frame
    if (arDetectMarker (dataPtr, threshold_, &marker_info, &marker_num) < 0)
    {
      argCleanup ();
      ROS_BREAK ();
    }

    int downsize = image_msg->width/cloud_width_;
    
    arPoseMarkers_.markers.clear ();
    // check for known patterns
    for (i = 0; i < objectnum; i++)
    {
      k = -1;
      for (j = 0; j < marker_num; j++)
      {
        if (object[i].id == marker_info[j].id)
        {
          if (k == -1)
            k = j;
          else                  // make sure you have the best pattern (highest confidence factor)
          if (marker_info[k].cf < marker_info[j].cf)
            k = j;
        }
      }
      if (k == -1)
      {
        object[i].visible = 0;
        continue;
      }
        
      // **** these are in the ROS frame
      double quat[4], pos[3];

      if (object[i].visible == 0)
      {
        arGetTransMat (&marker_info[k], object[i].marker_center, object[i].marker_width, object[i].trans);
      }
      else
      {
        arGetTransMatCont (&marker_info[k], object[i].trans,
                           object[i].marker_center, object[i].marker_width, object[i].trans);
      }
      object[i].visible = 1;

      double arQuat[4], arPos[3];
      //arUtilMatInv (object[i].trans, cam_trans);
      arUtilMat2QuatPos (object[i].trans, arQuat, arPos);

      pos[0] = arPos[0] * AR_TO_ROS;
      pos[1] = arPos[1] * AR_TO_ROS;
      pos[2] = arPos[2] * AR_TO_ROS;

      quat[0] = -arQuat[0];
      quat[1] = -arQuat[1];
      quat[2] = -arQuat[2];
      quat[3] = arQuat[3];

      if(gotcloud_){
        // Try to do high-definition via point clouds!
        pcl::PointXYZRGBNormal point = cloud_((int)  (marker_info[k].pos[0]/downsize), (int) (marker_info[k].pos[1]/downsize));
        if( ! (std::isnan(point.x) || std::isnan(point.y) || std::isnan(point.z)) ){
          pos[0] = point.x; 
          pos[1] = point.y; 
          pos[2] = point.z; 
            
          btQuaternion q = btQuaternion(quat[0],quat[1],quat[2],quat[3]);                   // quaternion rotation as found by ArToolkit
          btVector3 normal = btVector3(point.normal_x,point.normal_y,point.normal_z);       // normal vector from PCL
          btVector3 up = btTransform(q)*btVector3(0,0,1);                                   //

          // get axis perpendicular to normal and up vector?
          btVector3 axis = up.cross(normal);
          // and angle
          float angle = up.dot(normal);
          btQuaternion r = btQuaternion(axis, (btScalar) angle)*q;

          ROS_INFO("normal %f %f %f %f %f %f",normal.x(), normal.y(),normal.z(), up.x(), up.y(), up.z());
          quat[0] = (double) q.x();
          quat[1] = (double) q.y();
          quat[2] = (double) q.z();
          quat[3] = (double) q.w();
        }
      }

      // **** publish the marker

      ar_pose::ARMarker ar_pose_marker;
      ar_pose_marker.header.frame_id = image_msg->header.frame_id;
      ar_pose_marker.header.stamp = image_msg->header.stamp;
      ar_pose_marker.id = object[i].id;

      ar_pose_marker.pose.pose.position.x = pos[0];
      ar_pose_marker.pose.pose.position.y = pos[1];
      ar_pose_marker.pose.pose.position.z = pos[2];

      ar_pose_marker.pose.pose.orientation.x = quat[0];
      ar_pose_marker.pose.pose.orientation.y = quat[1];
      ar_pose_marker.pose.pose.orientation.z = quat[2];
      ar_pose_marker.pose.pose.orientation.w = quat[3];

      ar_pose_marker.confidence = marker_info->cf;
      arPoseMarkers_.markers.push_back (ar_pose_marker);

      // **** publish transform between camera and marker

      btQuaternion rotation (quat[0], quat[1], quat[2], quat[3]);
      btVector3 origin (pos[0], pos[1], pos[2]);
      btTransform t (rotation, origin);

      if (publishTf_)
      {
			tf::StampedTransform camToMarker (t, image_msg->header.stamp, image_msg->header.frame_id, object[i].name);
			broadcaster_.sendTransform(camToMarker);
      }

      // **** publish visual marker

      if (publishVisualMarkers_)
      {
        btVector3 markerOrigin (0, 0, 0.25 * object[i].marker_width * AR_TO_ROS);
        btTransform m (btQuaternion::getIdentity (), markerOrigin);
        btTransform markerPose = t * m; // marker pose in the camera frame

        tf::poseTFToMsg (markerPose, rvizMarker_.pose);

        rvizMarker_.header.frame_id = image_msg->header.frame_id;
        rvizMarker_.header.stamp = image_msg->header.stamp;
        rvizMarker_.id = object[i].id;

        rvizMarker_.scale.x = 1.0 * object[i].marker_width * AR_TO_ROS;
        rvizMarker_.scale.y = 1.0 * object[i].marker_width * AR_TO_ROS;
        rvizMarker_.scale.z = 0.5 * object[i].marker_width * AR_TO_ROS;
        rvizMarker_.ns = "basic_shapes";
        rvizMarker_.type = visualization_msgs::Marker::CUBE;
        rvizMarker_.action = visualization_msgs::Marker::ADD;
        switch (i)
        {
          case 0:
            rvizMarker_.color.r = 0.0f;
            rvizMarker_.color.g = 0.0f;
            rvizMarker_.color.b = 1.0f;
            rvizMarker_.color.a = 1.0;
            break;
          case 1:
            rvizMarker_.color.r = 1.0f;
            rvizMarker_.color.g = 0.0f;
            rvizMarker_.color.b = 0.0f;
            rvizMarker_.color.a = 1.0;
            break;
          default:
            rvizMarker_.color.r = 0.0f;
            rvizMarker_.color.g = 1.0f;
            rvizMarker_.color.b = 0.0f;
            rvizMarker_.color.a = 1.0;
        }
        rvizMarker_.lifetime = ros::Duration ();

        rvizMarkerPub_.publish (rvizMarker_);
        ROS_DEBUG ("Published visual marker");
      }
    }
    arMarkerPub_.publish (arPoseMarkers_);
    ROS_DEBUG ("Published ar_multi markers");
  }

  void ARPublisher::cloudCallback(const sensor_msgs::PointCloud2ConstPtr& msg)
  {
    // convert to PCL
    PointCloud temp;
    pcl::fromROSMsg(*msg, temp);
 
    // Compute surface normals and curvature
    pcl::NormalEstimation<pcl::PointXYZRGB, pcl::PointXYZRGBNormal> norm_est;
    norm_est.setSearchMethod (boost::make_shared<pcl::KdTreeFLANN<pcl::PointXYZRGB> > ());
    norm_est.setKSearch (25);
  
    norm_est.setInputCloud (temp.makeShared());
    pcl::copyPointCloud (temp, cloud_);
    norm_est.compute (cloud_);
    
    if(!gotcloud_){
      cloud_width_ = msg->width;
      std::cout << cloud_ << std::endl;
      pcl::PointXYZRGB point = temp(64,48);
      printf("%f %f %f\n", point.x , point.y , point.z);
      pcl::PointXYZRGBNormal point2 = cloud_(64,48);
      printf("%f %f %f %f %f %f\n", point2.x , point2.y , point2.z, point2.normal_x, point2.normal_y, point2.normal_z);
    }

    // can now use clouds
    gotcloud_ = true;
  }
}                               // end namespace ar_pose