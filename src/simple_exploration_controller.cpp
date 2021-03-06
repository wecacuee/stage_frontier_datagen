//
// Created by rakesh on 29/05/18.
//

//=================================================================================================
// Copyright (c) 2018, Rakesh Shrestha
// Acknowledgment: Stefan Kohlbrecher, TU Darmstadt
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the Simulation, Systems Optimization and Robotics
//       group, TU Darmstadt nor the names of its contributors may be used to
//       endorse or promote products derived from this software without
//       specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//=================================================================================================

#define MAX_ROBOT_TO_FIRST_WAYPOINT_DISTANCE 2

#include <stage_frontier_datagen/simple_exploration_controller.h>
namespace stage_frontier_datagen
{
SimpleExplorationController::SimpleExplorationController(const boost::function<void(const geometry_msgs::Twist&)> update_cmd_vel_functor)
  : update_cmd_vel_functor_(update_cmd_vel_functor),
    planner_(new hector_exploration_planner::HectorExplorationPlanner()),
    plan_update_callback_(0),
    is_planner_initialized_(false),
    is_planner_running_(false),
    is_plan_update_callback_running_(false),
    planner_status_(true)
{
  path_follower_.initialize(&tfl_);

  // TODO: update exploration plan a few waypoints before your previous plan will finish
//  exploration_plan_generation_timer_ = nh_.createTimer(ros::Duration(1.0),
//                                                       &SimpleExplorationController::timerPlanExploration, this, false);
//  cmd_vel_generator_timer_ = nh_.createTimer(ros::Duration(0.1), &SimpleExplorationController::timerCmdVelGeneration,
//                                            this, false);

  vel_pub_ = nh_.advertise<geometry_msgs::Twist>("cmd_vel", 10);
  exploration_plan_pub_ = nh_.advertise<nav_msgs::Path>("exploration_path", 2);
}

void SimpleExplorationController::startExploration()
{
  if (!costmap_2d_ros_)
  {
    initializeCostmap();
//    costmap_2d_ros_.reset(new costmap_2d::Costmap2DROS("", tfl_));
    planner_->initialize(ros::this_node::getNamespace(), costmap_2d_ros_.get());
  }

  exploration_plan_generation_timer_.start();
  cmd_vel_generator_timer_.start();
  is_planner_initialized_ = true;
  // should not use old plan if we reset the position of robot, hence set planner_status to false
  planner_status_ = false;
}

void SimpleExplorationController::stopExploration()
{
  exploration_plan_generation_timer_.stop();
  cmd_vel_generator_timer_.stop();

  geometry_msgs::Twist empty_vel;
  vel_pub_.publish(empty_vel);
  {
    boost::mutex::scoped_lock lock(update_cmd_vel_functor_mutex_);
    if (update_cmd_vel_functor_)
    {
      update_cmd_vel_functor_(empty_vel);
    }
  }

  clearCostmap();

  is_planner_initialized_ = false;

}

bool SimpleExplorationController::updatePlan()
{
  if (!is_planner_initialized_)
  {
    ROS_WARN_THROTTLE(1, "Planner not initialized");
    return false;
  }

  if (is_planner_running_)
  {
    return false;
  }

  tf::Stamped<tf::Pose> robot_pose_tf;
  bool robot_pose_status = costmap_2d_ros_->getRobotPose(robot_pose_tf);

  if (!robot_pose_status || robot_pose_tf.getRotation().length2() < 1e-5)
  {
    ROS_ERROR("Failed to get robot pose from costmap");
    return false;
  }

  geometry_msgs::PoseStamped pose;
  tf::poseStampedTFToMsg(robot_pose_tf, pose);

  tf::Quaternion orientation;
  tf::quaternionMsgToTF(pose.pose.orientation, orientation);
  if (orientation.length2() < 1e-5)
  {
    ROS_ERROR("poseStampedTFToMsg incorrect");
  }

  auto planner_thread = boost::thread([this, pose]() {
    nav_msgs::Path path;
    is_planner_running_ = true;
    ROS_INFO("running planner...");
    planner_status_ = planner_->doExploration(pose, path.poses);
    is_planner_running_ = false;

    if (path.poses.empty())
    {
      planner_status_ = false;
    }

    if (planner_status_)
    {
      if (std::hypot(path.poses[0].pose.position.x - pose.pose.position.x,
                     path.poses[0].pose.position.y - pose.pose.position.y) > MAX_ROBOT_TO_FIRST_WAYPOINT_DISTANCE)
      {
        planner_status_ = false;
        ROS_WARN("Invalid plan, first waypoint far away from robot");
      }
      else
      {
        updatePath(path);
        ROS_INFO("Generated exploration current_path_ with %u poses", (unsigned int) current_path_.poses.size());
        current_path_.header.frame_id = "map";
        current_path_.header.stamp = ros::Time::now();

        if (exploration_plan_pub_.getNumSubscribers() > 0)
        {
          exploration_plan_pub_.publish(current_path_);
        }
      }
    }
    else
    {
      ROS_INFO("planner failed");
    }

    // make sure that the previous callback finished before going to next
    if (plan_update_callback_)
    {
      if (!is_plan_update_callback_running_)
      {
        is_plan_update_callback_running_ = true;
        plan_update_callback_(*this, static_cast<bool>(planner_status_));
        is_plan_update_callback_running_ = false;
      }
      else
      {
        ROS_WARN("Skipping plan update callback because previous one hasn't finished");
      }

    }
  });

  planner_thread.detach();

  return true;
}

void SimpleExplorationController::timerPlanExploration(const ros::TimerEvent &e)
{
  planExploration();
}

void SimpleExplorationController::planExploration()
{
  if (!is_planner_running_)
  {
    updatePlan();
  }
}

void SimpleExplorationController::timerCmdVelGeneration(const ros::TimerEvent &e)
{
  generateCmdVel();
}

void SimpleExplorationController::generateCmdVel()
{
  // whether the callback is already running
  static boost::atomic_bool is_running(false);

  if (is_running)
  {
    return;
  }

  geometry_msgs::Twist twist;
  bool status = path_follower_.computeVelocityCommands(twist);

  if (!status || path_follower_.stopped() || path_follower_.isGoalReached() || !planner_status_)
  {
    is_running = true;

    // empty twist while planning
    geometry_msgs::Twist empty_vel;
    vel_pub_.publish(empty_vel);
    {
      boost::mutex::scoped_lock lock(update_cmd_vel_functor_mutex_);
      if (update_cmd_vel_functor_)
      {
        update_cmd_vel_functor_(empty_vel);
      }
    }

    if (!is_planner_running_)
    {
      updatePlan();
    }
  }
  else
  {
    // TODO: remove comment
    vel_pub_.publish(twist);
    {
      boost::mutex::scoped_lock lock(update_cmd_vel_functor_mutex_);
      if (update_cmd_vel_functor_)
      {
        update_cmd_vel_functor_(twist);
      }
    }
  }

  is_running = false;
}

void SimpleExplorationController::updatePath(nav_msgs::Path &path)
{
  boost::mutex::scoped_lock lock(path_mutex_);
  current_path_ = path;
  path_follower_.setPlan(path.poses);
}

nav_msgs::Path SimpleExplorationController::getCurrentPath()
{
  boost::mutex::scoped_lock lock(path_mutex_);
  return current_path_;
}

void SimpleExplorationController::clearCostmap()
{
  if (!costmap_2d_ros_)
  {
    ROS_ERROR("SimpleExplorationController: tried to clear non-existant costmap");
    return;
  }

  boost::unique_lock<costmap_2d::Costmap2D::mutex_t> lock(*(costmap_2d_ros_->getCostmap()->getMutex()));
  costmap_2d_ros_->resetLayers();
}

void SimpleExplorationController::updateCmdVelFunctor(const boost::function<void(const geometry_msgs::Twist&)> &update_cmd_vel_functor)
{
  boost::mutex::scoped_lock lock(update_cmd_vel_functor_mutex_);
  update_cmd_vel_functor_ = update_cmd_vel_functor;
}

void SimpleExplorationController::updateRobotOdom(const nav_msgs::OdometryConstPtr &odom)
{
  if (planner_ && costmap_2d_ros_)
  {
    costmap_2d_ros_->setRobotOdom(odom);
    path_follower_.setRobotOdom(odom);
  }
}

void SimpleExplorationController::initializeCostmap()
{
  costmap_2d_ros_.reset(new hector_exploration_planner::CustomCostmap2DROS("global_costmap", tfl_, false));

  boost::unique_lock<costmap_2d::Costmap2D::mutex_t> lock(*(costmap_2d_ros_->getCostmap()->getMutex()));
  for (const auto &layer: *(costmap_2d_ros_->getLayeredCostmap()->getPlugins()))
  {
    std::string layer_name = layer->getName();
    if (layer_name.find("ground_truth") != std::string::npos)
    {
      ground_truth_layer_ = boost::static_pointer_cast<ground_truth_layer::GroundTruthLayer>(layer);
      break;
    }
  }

  if (ground_truth_layer_)
  {
    // unsubscribe the topics. We want to updaet the data manually here
    ground_truth_layer_->deactivate();
  }
}

} // namespace stage_frontier_datagen

#include <costmap_2d/costmap_layer.h>
