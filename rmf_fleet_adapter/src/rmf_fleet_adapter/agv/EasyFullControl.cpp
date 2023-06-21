/*
 * Copyright (C) 2023 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <rmf_fleet_adapter/agv/Adapter.hpp>
#include <rmf_fleet_adapter/agv/EasyFullControl.hpp>
#include <rmf_fleet_adapter/agv/RobotCommandHandle.hpp>
#include <rmf_fleet_adapter/agv/Transformation.hpp>
#include "internal_EasyFullControl.hpp"
#include "internal_RobotUpdateHandle.hpp"
#include "internal_FleetUpdateHandle.hpp"

// Public rmf_task API headers
#include <rmf_task/Event.hpp>
#include <rmf_task/events/SimpleEventState.hpp>
#include <rmf_task/requests/ChargeBatteryFactory.hpp>
#include <rmf_task/requests/ParkRobotFactory.hpp>

// ROS2 utilities for rmf_traffic
#include <rmf_traffic/Trajectory.hpp>
#include <rmf_traffic/agv/Interpolate.hpp>
#include <rmf_traffic/agv/Planner.hpp>

#include <rmf_traffic/geometry/Circle.hpp>
#include <rmf_traffic_ros2/Time.hpp>

#include <rmf_battery/agv/BatterySystem.hpp>
#include <rmf_battery/agv/SimpleMotionPowerSink.hpp>
#include <rmf_battery/agv/SimpleDevicePowerSink.hpp>
#include <rmf_fleet_adapter/agv/parse_graph.hpp>

#include <thread>
#include <yaml-cpp/yaml.h>
#include <iostream>

//==============================================================================
namespace rmf_fleet_adapter {
namespace agv {

using ConsiderRequest = EasyFullControl::ConsiderRequest;
//==============================================================================
class EasyFullControl::CommandExecution::Implementation
{
public:

  struct StubbornOverride
  {
    /// We use a convoluted multi-layered reference structure for schedule
    /// override stubbornness so that we can release the stubbornness of the
    /// schedule override after the command is finished, even if the user
    /// forgets to release the override stubbornness.
    ///
    /// If we don't implement it like this, there's a risk that the agent will
    /// remain stubborn after it resumes normal operation, which would cause
    /// significant traffic management problems.
    std::shared_ptr<void> stubbornness;
  };

  struct ScheduleOverride
  {
    rmf_traffic::Route route;
    rmf_traffic::PlanId plan_id;
    std::weak_ptr<StubbornOverride> stubborn;
  };

  struct Data
  {
    std::vector<std::size_t> waypoints;
    std::vector<std::size_t> lanes;
    std::optional<double> final_orientation;
    std::optional<ScheduleOverride> schedule_override;
    std::shared_ptr<NavParams> nav_params;
    std::function<void(rmf_traffic::Duration)> arrival_estimator;

    void release_stubbornness()
    {
      if (schedule_override.has_value())
      {
        if (const auto stubborn = schedule_override->stubborn.lock())
        {
          // Clear out the previous stubborn handle
          stubborn->stubbornness = nullptr;
        }
      }
    }

    void update_location(
      const std::shared_ptr<RobotContext>& context,
      const std::string& map,
      Eigen::Vector3d location)
    {
      if (schedule_override.has_value())
      {
        return overridden_update(
          context,
          map,
          location,
          *schedule_override);
      }

      auto planner = context->planner();
      if (!planner)
      {
        RCLCPP_ERROR(
          context->node()->get_logger(),
          "Planner unavailable for robot [%s], cannot update its location",
          context->requester_id().c_str());
        return;
      }

      const auto& graph = planner->get_configuration().graph();
      const auto& closed_lanes = planner->get_configuration().lane_closures();
      std::optional<std::pair<std::size_t, double>> on_waypoint;
      auto p = Eigen::Vector2d(location[0], location[1]);
      const double yaw = location[2];
      for (std::size_t wp : waypoints)
      {
        if (wp >= graph.num_waypoints())
        {
          RCLCPP_ERROR(
            context->node()->get_logger(),
            "Robot [%s] has a command with a waypoint [%lu] that is outside "
            "the range of the graph [%lu]. We will not do a location update.",
            context->requester_id().c_str(),
            wp,
            graph.num_waypoints());
          // Should we also issue a replan command?
          return;
        }

        const auto p_wp = graph.get_waypoint(wp).get_location();
        auto dist = (p - p_wp).norm();
        if (dist <= nav_params->max_merge_waypoint_distance)
        {
          if (!on_waypoint.has_value() || dist < on_waypoint->second)
          {
            on_waypoint = std::make_pair(wp, dist);
          }
        }
      }

      rmf_traffic::agv::Plan::StartSet starts;
      const auto now = rmf_traffic_ros2::convert(context->node()->now());
      if (on_waypoint.has_value())
      {
        const auto wp = on_waypoint->first;
        starts.push_back(rmf_traffic::agv::Plan::Start(now, wp, yaw, p));
        for (std::size_t lane_id : graph.lanes_from(wp))
        {
          if (lane_id >= graph.num_lanes())
          {
            RCLCPP_ERROR(
              context->node()->get_logger(),
              "Nav graph for robot [%s] has an invalid lane ID [%lu] leaving "
              "vertex [%lu], lane ID range is [%lu]. We will not do a location "
              "update.",
              context->requester_id().c_str(),
              lane_id,
              wp,
              graph.num_lanes());
            // Should we also issue a replan command?
            return;
          }

          if (closed_lanes.is_closed(lane_id))
          {
            // Don't use a lane that's closed
            continue;
          }

          auto wp_exit = graph.get_lane(lane_id).exit().waypoint_index();
          starts.push_back(
            rmf_traffic::agv::Plan::Start(now, wp_exit, yaw, p, lane_id));
        }
      }
      else
      {
        std::optional<std::pair<std::size_t, double>> on_lane;
        for (auto lane_id : lanes)
        {
          if (lane_id >= graph.num_lanes())
          {
            RCLCPP_ERROR(
              context->node()->get_logger(),
              "Robot [%s] has a command with a lane [%lu] that is outside the "
              "range of the graph [%lu]. We will not do a location update.",
              context->requester_id().c_str(),
              lane_id,
              graph.num_lanes());
            // Should we also issue a replan command?
            return;
          }

          if (closed_lanes.is_closed(lane_id))
          {
            continue;
          }

          const auto& lane = graph.get_lane(lane_id);
          const auto p0 = graph.get_waypoint(lane.entry().waypoint_index()).get_location();
          const auto p1 = graph.get_waypoint(lane.exit().waypoint_index()).get_location();
          const auto lane_length = (p1 - p0).norm();
          const auto lane_u = (p1 - p0)/lane_length;
          const auto proj = (p - p0).dot(lane_u);
          if (proj < 0.0 || lane_length < proj)
          {
            continue;
          }

          const auto dist_to_lane = (p - p0 - proj * lane_u).norm();
          if (dist_to_lane <= nav_params->max_merge_lane_distance)
          {
            if (!on_lane.has_value() || dist_to_lane < on_lane->second)
            {
              on_lane = std::make_pair(lane_id, dist_to_lane);
            }
          }
        }

        if (on_lane.has_value())
        {
          const auto lane_id = on_lane->first;
          const auto& lane = graph.get_lane(lane_id);
          const auto wp0 = lane.entry().waypoint_index();
          const auto wp1 = lane.exit().waypoint_index();
          starts.push_back(
            rmf_traffic::agv::Plan::Start(now, wp1, yaw, p, lane_id));

          if (const auto* reverse_lane = graph.lane_from(wp1, wp0))
          {
            starts.push_back(rmf_traffic::agv::Plan::Start(
                now, wp0, yaw, p, reverse_lane->index()));
          }
        }
        else
        {
          starts = rmf_traffic::agv::compute_plan_starts(
            graph,
            map,
            location,
            now,
            nav_params->max_merge_waypoint_distance,
            nav_params->max_merge_lane_distance,
            nav_params->min_lane_length);
        }
      }

      context->set_location(starts);
      if (!waypoints.empty())
      {
        const auto p_final = graph.get_waypoint(waypoints.back()).get_location();
        const auto distance = (p_final - p).norm();
        double rotation = 0.0;
        if (final_orientation.has_value())
        {
          rotation = std::fabs(location[2] - *final_orientation);
        }

        const auto& traits = planner->get_configuration().vehicle_traits();
        const auto v = std::max(traits.linear().get_nominal_velocity(), 0.001);
        const auto w = std::max(traits.rotational().get_nominal_velocity(), 0.001);
        const auto t = distance / v + rotation / w;
        arrival_estimator(rmf_traffic::time::from_seconds(t));
      }
    }

    void overridden_update(
      const std::shared_ptr<RobotContext>& context,
      const std::string& map,
      Eigen::Vector3d location,
      const ScheduleOverride& schedule_override)
    {
      auto p = Eigen::Vector2d(location[0], location[1]);
      const auto& route = schedule_override.route;
      const auto plan_id = schedule_override.plan_id;
      std::optional<std::pair<std::size_t, double>> closest_lane;
      std::size_t i0 = 0;
      std::size_t i1 = 1;
      for (; i1 < route.trajectory().size(); ++i0, ++i1)
      {
        // We approximate the trajectory as linear with constant velocity even
        // though it could technically be a cubic spline. The linear
        // approximation simplifies the math considerably, and we will be
        // phasing out support for cubic splines in the future.
        const Eigen::Vector2d p0 =
          route.trajectory().at(i0).position().block<2, 1>(0, 0);
        const Eigen::Vector2d p1 =
          route.trajectory().at(i1).position().block<2, 1>(0, 0);
        const auto lane_length = (p1 - p0).norm();
        const auto lane_u = (p1 - p0)/lane_length;
        const auto proj = (p - p0).dot(lane_u);
        if (proj < 0.0 || lane_length < proj)
        {
          continue;
        }

        const auto dist_to_lane = (p - p0 - proj * lane_u).norm();
        if (!closest_lane.has_value() || dist_to_lane < closest_lane->second)
        {
          closest_lane = std::make_pair(i0, dist_to_lane);
        }
      }

      const auto now = rmf_traffic_ros2::convert(context->node()->now());
      const auto delay_thresh = std::chrono::seconds(1);
      if (closest_lane.has_value())
      {
        const auto& wp0 = route.trajectory().at(closest_lane->first);
        const auto& wp1 = route.trajectory().at(closest_lane->first + 1);
        const Eigen::Vector2d p0 = wp0.position().block<2, 1>(0, 0);
        const Eigen::Vector2d p1 = wp1.position().block<2, 1>(0, 0);
        const auto lane_length = (p1 - p0).norm();
        const auto lane_u = (p1 - p0)/lane_length;
        const auto proj = (p - p0).dot(lane_u);
        const auto s = proj/lane_length;
        const double dt = rmf_traffic::time::to_seconds(wp1.time() - wp0.time());
        const rmf_traffic::Time t_expected =
          wp0.time() + rmf_traffic::time::from_seconds(s*dt);
        const auto delay = now - t_expected;
        context->itinerary().cumulative_delay(plan_id, delay, delay_thresh);
      }
      else
      {
        // Find the waypoint that the agent is closest to and estimate the delay
        // based on the agent being at that waypoint. This is a very fallible
        // estimation, but it's the best we can do with limited information.
        std::optional<std::pair<rmf_traffic::Time, double>> closest_time;
        for (std::size_t i=0; i < route.trajectory().size(); ++i)
        {
          const auto& wp = route.trajectory().at(i);
          const Eigen::Vector2d p_wp = wp.position().block<2, 1>(0, 0);
          const double dist = (p - p_wp).norm();
          if (!closest_time.has_value() || dist < closest_time->second)
          {
            closest_time = std::make_pair(wp.time(), dist);
          }
        }

        if (closest_time.has_value())
        {
          const auto delay = now - closest_time->first;
          context->itinerary().cumulative_delay(plan_id, delay, delay_thresh);
        }

        // If no closest time was found then there are no waypoints in the
        // route. There's no point updating the delay of an empty route.
      }

      auto planner = context->planner();
      if (!planner)
      {
        RCLCPP_ERROR(
          context->node()->get_logger(),
          "Planner unavailable for robot [%s], cannot update its location",
          context->requester_id().c_str());
        return;
      }

      const auto& graph = planner->get_configuration().graph();
      auto starts = rmf_traffic::agv::compute_plan_starts(
        graph,
        map,
        location,
        now,
        nav_params->max_merge_waypoint_distance,
        nav_params->max_merge_lane_distance,
        nav_params->min_lane_length);
      context->set_location(std::move(starts));
    }
  };
  using DataPtr = std::shared_ptr<Data>;

  std::weak_ptr<RobotContext> w_context;
  std::shared_ptr<Data> data;
  std::function<void(CommandExecution)> begin;
  std::function<void()> finisher;
  ActivityIdentifierPtr identifier;

  void finish()
  {
    if (auto context = w_context.lock())
    {
      context->worker().schedule(
        [
          context = context,
          data = this->data,
          identifier = this->identifier,
          finisher = this->finisher
        ](const auto&)
        {
          if (!ActivityIdentifier::Implementation::get(*identifier).update_fn)
          {
            // This activity has already finished
            return;
          }

          // Prevent this activity from doing any further updates
          ActivityIdentifier::Implementation::get(*identifier).update_fn = nullptr;
          if (data->schedule_override.has_value())
          {
            data->release_stubbornness();
            context->request_replan();
          }
          else
          {
            // Trigger the next step in the sequence
            finisher();
          }
        });
    }
  }

  Stubbornness override_schedule(
    std::string map,
    std::vector<Eigen::Vector3d> path)
  {
    auto stubborn = std::make_shared<StubbornOverride>();
    if (const auto context = w_context.lock())
    {
      context->worker().schedule(
        [
          context,
          stubborn,
          data = this->data,
          identifier = this->identifier,
          map = std::move(map),
          path = std::move(path)
        ](const auto&)
        {
          if (!ActivityIdentifier::Implementation::get(*identifier).update_fn)
          {
            // Don't do anything because this command is finished
            return;
          }

          auto planner = context->planner();
          if (!planner)
          {
            RCLCPP_WARN(
              context->node()->get_logger(),
              "Planner unavailable for robot [%s], cannot override its "
              "schedule",
              context->requester_id().c_str());
            return;
          }

          data->release_stubbornness();
          const auto now = context->now();
          const auto& traits = planner->get_configuration().vehicle_traits();
          auto trajectory = rmf_traffic::agv::Interpolate::positions(
            traits, now, path);
          auto route = rmf_traffic::Route(map, std::move(trajectory));
          const auto plan_id = context->itinerary().assign_plan_id();
          context->itinerary().set(plan_id, {route});

          data->schedule_override = ScheduleOverride{
            std::move(route),
            plan_id,
            stubborn
          };

          stubborn->stubbornness = context->be_stubborn();
        });
    }

    return Stubbornness::Implementation::make(stubborn);
  }

  static CommandExecution make(
    const std::shared_ptr<RobotContext>& context_,
    Data data_,
    std::function<void(CommandExecution)> begin)
  {
    auto data = std::make_shared<Data>(data_);
    std::weak_ptr<RobotContext> context = context_;
    auto update_fn = [context, data](
        const std::string& map,
        Eigen::Vector3d location)
      {
        if (auto locked_context = context.lock())
        {
          data->update_location(locked_context, map, location);
        }
      };
    auto identifier = ActivityIdentifier::Implementation::make(update_fn);

    CommandExecution cmd;
    cmd._pimpl = rmf_utils::make_impl<Implementation>(
      Implementation{context, data, begin, nullptr, identifier});
    return cmd;
  }

  static Implementation& get(CommandExecution& cmd)
  {
    return *cmd._pimpl;
  }
};

//==============================================================================
void EasyFullControl::CommandExecution::finished()
{
  _pimpl->finish();
}

//==============================================================================
bool EasyFullControl::CommandExecution::okay() const
{
  if (!_pimpl->identifier)
  {
    return false;
  }

  if (!ActivityIdentifier::Implementation::get(*_pimpl->identifier).update_fn)
  {
    return false;
  }

  return true;
}

//==============================================================================
auto EasyFullControl::CommandExecution::override_schedule(
  std::string map,
  std::vector<Eigen::Vector3d> path) -> Stubbornness
{
  return _pimpl->override_schedule(std::move(map), std::move(path));
}

//==============================================================================
auto EasyFullControl::CommandExecution::identifier() const
-> ConstActivityIdentifierPtr
{
  return _pimpl->identifier;
}

//==============================================================================
EasyFullControl::CommandExecution::CommandExecution()
{
  // Do nothing
}

//==============================================================================
EasyFullControl::EasyFullControl()
{
  // Do nothing
}

//==============================================================================
class EasyFullControl::Destination::Implementation
{
public:
  std::string map;
  Eigen::Vector3d position;
  std::optional<std::size_t> graph_index;

  static Destination make(
    std::string map,
    Eigen::Vector3d position,
    std::optional<std::size_t> graph_index)
  {
    Destination output;
    output._pimpl = rmf_utils::make_impl<Implementation>(
      Implementation{
        std::move(map),
        position,
        graph_index
      });
    return output;
  }
};

//==============================================================================
const std::string& EasyFullControl::Destination::map() const
{
  return _pimpl->map;
}

//==============================================================================
Eigen::Vector3d EasyFullControl::Destination::position() const
{
  return _pimpl->position;
}

//==============================================================================
Eigen::Vector2d EasyFullControl::Destination::xy() const
{
  return _pimpl->position.block<2, 1>(0, 0);
}

//==============================================================================
double EasyFullControl::Destination::yaw() const
{
  return _pimpl->position[2];
}

//==============================================================================
std::optional<std::size_t> EasyFullControl::Destination::graph_index() const
{
  return _pimpl->graph_index;
}

//==============================================================================
EasyFullControl::Destination::Destination()
{
  // Do nothing
}

//==============================================================================
struct ProgressTracker : std::enable_shared_from_this<ProgressTracker>
{
  /// The queue of commands to execute while following this path, in reverse
  /// order so that the next command can always be popped off the back.
  std::vector<EasyFullControl::CommandExecution> reverse_queue;
  EasyFullControl::ActivityIdentifierPtr current_identifier;
  TriggerOnce finished;

  void next()
  {
    if (reverse_queue.empty())
    {
      current_identifier = nullptr;
      finished.trigger();
      return;
    }

    auto current_activity = reverse_queue.back();
    reverse_queue.pop_back();
    current_identifier = EasyFullControl::CommandExecution::Implementation
      ::get(current_activity).identifier;
    auto& current_activity_impl =
      EasyFullControl::CommandExecution::Implementation::get(current_activity);
    current_activity_impl.finisher = [w_progress = weak_from_this()]()
      {
        if (const auto progress = w_progress.lock())
        {
          progress->next();
        }
      };
    const auto begin = current_activity_impl.begin;
    if (begin)
    {
      begin(std::move(current_activity));
    }
  }

  static std::shared_ptr<ProgressTracker> make(
    std::vector<EasyFullControl::CommandExecution> queue,
    std::function<void()> finished)
  {
    std::reverse(queue.begin(), queue.end());
    auto tracker = std::make_shared<ProgressTracker>();
    tracker->reverse_queue = std::move(queue);
    tracker->finished = TriggerOnce(std::move(finished));
    return tracker;
  }
};

//==============================================================================
/// Implements a state machine to send waypoints from follow_new_path() one
/// at a time to the robot via its API. Also updates state of robot via a timer.
class EasyCommandHandle : public RobotCommandHandle,
  public std::enable_shared_from_this<EasyCommandHandle>
{
public:
  using Planner = rmf_traffic::agv::Planner;
  using Graph = rmf_traffic::agv::Graph;
  using VehicleTraits = rmf_traffic::agv::VehicleTraits;
  using ActionExecution = RobotUpdateHandle::ActionExecution;
  using ActionExecutor = RobotUpdateHandle::ActionExecutor;
  using InitializeRobot = EasyFullControl::InitializeRobot;
  using NavigationRequest = EasyFullControl::NavigationRequest;
  using StopRequest = EasyFullControl::StopRequest;
  using DockRequest = EasyFullControl::DockRequest;
  using Status = rmf_task::Event::Status;
  using ActivityIdentifierPtr = EasyFullControl::ActivityIdentifierPtr;

  // State machine values.
  enum class InternalRobotState : uint8_t
  {
    IDLE = 0,
    MOVING = 1
  };

  EasyCommandHandle(
    std::shared_ptr<NavParams> nav_params,
    NavigationRequest handle_nav_request,
    StopRequest handle_stop,
    DockRequest handle_dock);

  // Implement base class methods.
  void stop() final;

  void follow_new_path(
    const std::vector<rmf_traffic::agv::Plan::Waypoint>& waypoints,
    ArrivalEstimator next_arrival_estimator,
    RequestCompleted path_finished_callback) final;

  void dock(
    const std::string& dock_name,
    RequestCompleted docking_finished_callback) final;

  std::weak_ptr<RobotContext> w_context;
  std::shared_ptr<NavParams> nav_params;
  std::shared_ptr<ProgressTracker> current_progress;

  // Callbacks from user
  NavigationRequest handle_nav_request;
  StopRequest handle_stop;
  DockRequest handle_dock;
};

//==============================================================================
EasyCommandHandle::EasyCommandHandle(
  std::shared_ptr<NavParams> nav_params_,
  NavigationRequest handle_nav_request_,
  StopRequest handle_stop_,
  DockRequest handle_dock_)
: nav_params(std::move(nav_params_)),
  handle_nav_request(std::move(handle_nav_request_)),
  handle_stop(std::move(handle_stop_)),
  handle_dock(std::move(handle_dock_))
{
  // Do nothing
}

//==============================================================================
void EasyCommandHandle::stop()
{
  if (!this->current_progress)
  {
    return;
  }

  const auto activity_identifier = this->current_progress->current_identifier;
  if (!activity_identifier)
  {
    return;
  }

  /// Prevent any further specialized updates.
  EasyFullControl::ActivityIdentifier::Implementation
    ::get(*activity_identifier).update_fn = nullptr;

  this->current_progress = nullptr;
  this->handle_stop(activity_identifier);
}

//==============================================================================
void EasyCommandHandle::follow_new_path(
  const std::vector<rmf_traffic::agv::Plan::Waypoint>& waypoints,
  ArrivalEstimator next_arrival_estimator_,
  RequestCompleted path_finished_callback_)
{
  const auto context = w_context.lock();
  if (!context)
  {
    return;
  }

  RCLCPP_DEBUG(
    context->node()->get_logger(),
    "follow_new_path for robot [%s] with PlanId [%ld]",
    context->requester_id().c_str(),
    context->itinerary().current_plan_id());

  if (waypoints.empty() ||
    next_arrival_estimator_ == nullptr ||
    path_finished_callback_ == nullptr)
  {
    RCLCPP_WARN(
      context->node()->get_logger(),
      "Received a new path for robot [%s] with invalid parameters. "
      " Ignoring...",
      context->requester_id().c_str());
    return;
  }

  const auto& planner = context->planner();
  if (!planner)
  {
    RCLCPP_ERROR(
      context->node()->get_logger(),
      "Planner missing for [%s], cannot follow new path commands",
      context->requester_id().c_str());
    return;
  }
  const auto& graph = planner->get_configuration().graph();
  std::optional<std::string> opt_initial_map;
  for (const auto& wp : waypoints)
  {
    if (wp.graph_index().has_value())
    {
      const std::size_t i = *wp.graph_index();
      opt_initial_map = graph.get_waypoint(i).get_map_name();
      break;
    }
  }

  if (!opt_initial_map.has_value())
  {
    RCLCPP_ERROR(
      context->node()->get_logger(),
      "Could not find an initial map in follow_new_path command for robot [%s]."
      " This is an internal RMF error, please report it to the developers. "
      "Path length is [%lu].",
      context->requester_id().c_str(),
      waypoints.size());
    return;
  }
  std::string initial_map = *opt_initial_map;

  std::vector<EasyFullControl::CommandExecution> queue;
  const auto& current_location = context->location();

  bool found_connection = false;
  std::size_t i0 = 0;
  for (std::size_t i = 0; i < waypoints.size(); ++i)
  {
    const auto& wp = waypoints[i];
    if (wp.graph_index().has_value())
    {
      for (const auto& l : current_location)
      {
        if (*wp.graph_index() == l.waypoint())
        {
          found_connection = true;
          i0 = i;
        }
      }
    }

    if (i > 0)
    {
      for (const auto lane : wp.approach_lanes())
      {
        for (const auto& l : current_location)
        {
          if (l.lane().has_value())
          {
            if (lane == *l.lane())
            {
              found_connection = true;
              i0 = i - 1;
            }
          }
        }
      }
    }
  }

  if (!found_connection)
  {
    // The robot has drifted away from the starting point since the plan started
    // so we'll ask for a new plan.
    context->request_replan();
    return;
  }

  if (i0 >= waypoints.size() - 1)
  {
    // Always issue at least one command to approach the final waypoint.
    i0 = waypoints.size() - 2;
  }

  std::size_t i1 = i0 + 1;
  for (; i1 < waypoints.size(); ++i0, ++i1)
  {
    // TODO(@mxgrey): Add an option to discard waypoints that are only doing a
    // rotation.
    std::vector<std::size_t> cmd_wps;
    std::vector<std::size_t> cmd_lanes;
    const auto& wp0 = waypoints[i0];
    const auto& wp1 = waypoints[i1];
    if (wp0.graph_index().has_value())
    {
      cmd_wps.push_back(*wp0.graph_index());
    }

    for (auto lane_id : wp1.approach_lanes())
    {
      cmd_lanes.push_back(lane_id);
      const auto& lane = graph.get_lane(lane_id);
      const std::size_t entry_wp = lane.entry().waypoint_index();
      const std::size_t exit_wp = lane.exit().waypoint_index();
      for (auto wp : {entry_wp, exit_wp})
      {
        if (std::find(cmd_wps.begin(), cmd_wps.end(), wp) == cmd_wps.end())
        {
          cmd_wps.push_back(wp);
        }
      }
    }

    std::string map = [&]()
      {
        if (wp1.graph_index().has_value())
        {
          return graph.get_waypoint(*wp1.graph_index()).get_map_name();
        }

        return initial_map;
      }();
    if (initial_map != map)
    {
      initial_map = map;
    }

    Eigen::Vector3d target_position = wp1.position();
    std::size_t target_index = i1;
    bool skip_next = false;
    if (nav_params->skip_rotation_commands)
    {
      const std::size_t i2 = i1 + 1;
      if (i2 < waypoints.size())
      {
        const auto& wp2 = waypoints[i2];
        if (wp1.graph_index().has_value() && wp2.graph_index().has_value())
        {
          if (*wp1.graph_index() == *wp2.graph_index())
          {
            target_index = i2;
            target_position = wp2.position();
            skip_next = true;
          }
        }
      }
    }

    auto destination = EasyFullControl::Destination::Implementation::make(
      std::move(map),
      target_position,
      wp1.graph_index());

    queue.push_back(
      EasyFullControl::CommandExecution::Implementation::make(
        context,
        EasyFullControl::CommandExecution::Implementation::Data{
          cmd_wps,
          cmd_lanes,
          target_position[2],
          std::nullopt,
          nav_params,
          [next_arrival_estimator_, target_index](rmf_traffic::Duration dt)
          {
            next_arrival_estimator_(target_index, dt);
          }
        },
        [
          handle_nav_request = this->handle_nav_request,
          destination = std::move(destination)
        ](EasyFullControl::CommandExecution execution)
        {
          handle_nav_request(destination, execution);
        }
      ));

    if (skip_next)
    {
      ++i0;
      ++i1;
    }
  }

  this->current_progress = ProgressTracker::make(
    queue,
    path_finished_callback_);
  this->current_progress->next();
}

//==============================================================================
namespace {
class DockFinder : public rmf_traffic::agv::Graph::Lane::Executor
{
public:
  DockFinder(std::string dock_name)
  : looking_for(std::move(dock_name))
  {
    // Do nothing
  }

  void execute(const DoorOpen&) override {}
  void execute(const DoorClose&) override {}
  void execute(const LiftSessionBegin&) override {}
  void execute(const LiftDoorOpen&) override {}
  void execute(const LiftSessionEnd&) override {}
  void execute(const LiftMove&) override {}
  void execute(const Wait&) override {}
  void execute(const Dock& dock) override
  {
    if (looking_for == dock.dock_name())
    {
      found = true;
    }
  }

  std::string looking_for;
  bool found = false;
};
}

//==============================================================================
void EasyCommandHandle::dock(
  const std::string& dock_name_,
  RequestCompleted docking_finished_callback_)
{
  const auto context = w_context.lock();
  if (!context)
  {
    return;
  }

  RCLCPP_DEBUG(
    context->node()->get_logger(),
    "Received a request to dock robot [%s] at [%s]...",
    context->requester_id().c_str(),
    dock_name_.c_str());

  const auto plan_id = context->itinerary().current_plan_id();
  auto planner = context->planner();
  if (!planner)
  {
    RCLCPP_ERROR(
      context->node()->get_logger(),
      "Planner unavailable for robot [%s], cannot execute docking command [%s]",
      context->requester_id().c_str(),
      dock_name_.c_str());
    return;
  }

  const auto& graph = planner->get_configuration().graph();
  DockFinder finder(dock_name_);
  std::optional<std::size_t> found_lane;
  for (std::size_t i = 0; i < graph.num_lanes(); ++i)
  {
    const auto& lane = graph.get_lane(i);
    if (const auto event = lane.entry().event())
    {
      event->execute(finder);
    }

    if (const auto event = lane.exit().event())
    {
      event->execute(finder);
    }

    if (finder.found)
    {
      found_lane = i;
      break;
    }
  }

  auto data = [&]()
    {
      if (!found_lane.has_value())
      {
        RCLCPP_WARN(
          context->node()->get_logger(),
          "Unable to find a dock named [%s] in the graph for robot [%s], cannot "
          "perform position updates correctly.",
          dock_name_.c_str(),
          context->requester_id().c_str());
        return EasyFullControl::CommandExecution::Implementation::Data{
          {},
          {},
          std::nullopt,
          std::nullopt,
          nav_params,
          [](rmf_traffic::Duration) { }
        };
      }
      else
      {
        const auto& lane = graph.get_lane(*found_lane);
        const std::size_t i0 = lane.entry().waypoint_index();
        const std::size_t i1 = lane.exit().waypoint_index();
        const auto& wp0 = graph.get_waypoint(i0);
        const auto& wp1 = graph.get_waypoint(i1);
        const Eigen::Vector2d p0 = wp0.get_location();
        const Eigen::Vector2d p1 = wp1.get_location();
        const double dist = (p1 - p0).norm();
        const auto& traits = planner->get_configuration().vehicle_traits();
        const double v = std::max(traits.linear().get_nominal_velocity(), 0.001);
        const double dt = dist / v;
        const rmf_traffic::Time expected_arrival =
          context->now() + rmf_traffic::time::from_seconds(dt);

        return EasyFullControl::CommandExecution::Implementation::Data{
          {i0, i1},
          {*found_lane},
          std::nullopt,
          std::nullopt,
          nav_params,
          [w_context = context->weak_from_this(), expected_arrival, plan_id](
            rmf_traffic::Duration dt)
          {
            const auto context = w_context.lock();
            if (!context)
            {
              return;
            }

            const rmf_traffic::Time now = context->now();
            const auto updated_arrival = now + dt;
            const auto delay = updated_arrival - expected_arrival;
            context->itinerary().cumulative_delay(
              plan_id, delay, std::chrono::seconds(1));
          }
        };
      }
    }();

  auto cmd = EasyFullControl::CommandExecution::Implementation::make(
    context,
    data,
    [handle_dock = this->handle_dock, dock_name = dock_name_](
      EasyFullControl::CommandExecution execution)
    {
      handle_dock(dock_name, execution);
    });
  this->current_progress = ProgressTracker::make(
    {std::move(cmd)},
    std::move(docking_finished_callback_));
  this->current_progress->next();
}

//==============================================================================
ConsiderRequest consider_all()
{
  return [](const nlohmann::json&, FleetUpdateHandle::Confirmation& confirm)
    {
      confirm.accept();
    };
}

//==============================================================================
class EasyFullControl::EasyRobotUpdateHandle::Implementation
{
public:

  struct Updater
  {
    std::shared_ptr<RobotUpdateHandle> handle;
    std::shared_ptr<NavParams> params;

    Updater(std::shared_ptr<NavParams> params_)
    : handle(nullptr),
      params(std::move(params_))
    {
      // Do nothing
    }
  };

  std::shared_ptr<Updater> updater;
  rxcpp::schedulers::worker worker;

  static Implementation& get(EasyRobotUpdateHandle& handle)
  {
    return *handle._pimpl;
  }

  Implementation(
    std::shared_ptr<NavParams> params_,
    rxcpp::schedulers::worker worker_)
  : updater(std::make_shared<Updater>(params_)),
    worker(worker_)
  {
    // Do nothing
  }

  static std::shared_ptr<EasyRobotUpdateHandle> make(
    std::shared_ptr<NavParams> params_,
    rxcpp::schedulers::worker worker_)
  {
    auto handle = std::shared_ptr<EasyRobotUpdateHandle>(new EasyRobotUpdateHandle);
    handle->_pimpl = rmf_utils::make_unique_impl<Implementation>(
      std::move(params_), std::move(worker_));
    return handle;
  }
};

//==============================================================================
void EasyFullControl::EasyRobotUpdateHandle::update_position(
  std::string map_name,
  Eigen::Vector3d position,
  ConstActivityIdentifierPtr current_activity)
{
  _pimpl->worker.schedule(
    [
      map_name = std::move(map_name),
      position,
      current_activity = std::move(current_activity),
      updater = _pimpl->updater
    ](const auto&)
    {
      if (current_activity)
      {
        ActivityIdentifier::Implementation::get(*current_activity).update_fn(
          map_name, position);
        return;
      }

      auto context = RobotUpdateHandle::Implementation
        ::get(*updater->handle).get_context();
      auto planner = context->planner();
      if (!planner)
      {
        RCLCPP_ERROR(
          context->node()->get_logger(),
          "Planner unavailable for robot [%s], cannot update its location",
          context->requester_id().c_str());
        return;
      }
      const auto& graph = planner->get_configuration().graph();
      const auto& nav_params = updater->params;
      const auto now = context->now();

      auto starts = rmf_traffic::agv::compute_plan_starts(
        graph,
        map_name,
        position,
        now,
        nav_params->max_merge_waypoint_distance,
        nav_params->max_merge_lane_distance,
        nav_params->min_lane_length);
    });
}

//==============================================================================
void EasyFullControl::EasyRobotUpdateHandle::update_battery_soc(double soc)
{
  if (_pimpl->updater && _pimpl->updater->handle)
  {
    _pimpl->updater->handle->update_battery_soc(soc);
  }
}

//==============================================================================
std::shared_ptr<RobotUpdateHandle>
EasyFullControl::EasyRobotUpdateHandle::more()
{
  if (_pimpl->updater)
  {
    return _pimpl->updater->handle;
  }

  return nullptr;
}

//==============================================================================
std::shared_ptr<const RobotUpdateHandle>
EasyFullControl::EasyRobotUpdateHandle::more() const
{
  if (_pimpl->updater)
  {
    return _pimpl->updater->handle;
  }

  return nullptr;
}

//==============================================================================
EasyFullControl::EasyRobotUpdateHandle::EasyRobotUpdateHandle()
{
  // Do nothing
}

//==============================================================================
class EasyFullControl::Configuration::Implementation
{
public:
  std::string fleet_name;
  std::shared_ptr<const rmf_traffic::agv::VehicleTraits> traits;
  std::shared_ptr<const rmf_traffic::agv::Graph> graph;
  rmf_battery::agv::ConstBatterySystemPtr battery_system;
  rmf_battery::ConstMotionPowerSinkPtr motion_sink;
  rmf_battery::ConstDevicePowerSinkPtr ambient_sink;
  rmf_battery::ConstDevicePowerSinkPtr tool_sink;
  double recharge_threshold;
  double recharge_soc;
  bool account_for_battery_drain;
  std::unordered_map<std::string, ConsiderRequest> task_consideration;
  std::unordered_map<std::string, ConsiderRequest> action_consideration;
  rmf_task::ConstRequestFactoryPtr finishing_request;
  bool skip_rotation_commands;
  std::optional<std::string> server_uri;
  rmf_traffic::Duration max_delay;
  rmf_traffic::Duration update_interval;
};

//==============================================================================
EasyFullControl::Configuration::Configuration(
  const std::string& fleet_name,
  std::shared_ptr<const rmf_traffic::agv::VehicleTraits> traits,
  std::shared_ptr<const rmf_traffic::agv::Graph> graph,
  rmf_battery::agv::ConstBatterySystemPtr battery_system,
  rmf_battery::ConstMotionPowerSinkPtr motion_sink,
  rmf_battery::ConstDevicePowerSinkPtr ambient_sink,
  rmf_battery::ConstDevicePowerSinkPtr tool_sink,
  double recharge_threshold,
  double recharge_soc,
  bool account_for_battery_drain,
  std::unordered_map<std::string, ConsiderRequest> task_consideration,
  std::unordered_map<std::string, ConsiderRequest> action_consideration,
  rmf_task::ConstRequestFactoryPtr finishing_request,
  bool skip_rotation_commands,
  std::optional<std::string> server_uri,
  rmf_traffic::Duration max_delay,
  rmf_traffic::Duration update_interval)
: _pimpl(rmf_utils::make_impl<Implementation>(
      Implementation{
        std::move(fleet_name),
        std::move(traits),
        std::move(graph),
        std::move(battery_system),
        std::move(motion_sink),
        std::move(ambient_sink),
        std::move(tool_sink),
        std::move(recharge_threshold),
        std::move(recharge_soc),
        std::move(account_for_battery_drain),
        std::move(task_consideration),
        std::move(action_consideration),
        std::move(finishing_request),
        skip_rotation_commands,
        std::move(server_uri),
        std::move(max_delay),
        std::move(update_interval)
      }))
{
  // Do nothing
}

//==============================================================================
std::shared_ptr<EasyFullControl::Configuration>
EasyFullControl::Configuration::from_config_files(
  const std::string& config_file,
  const std::string& nav_graph_path,
  std::optional<std::string> server_uri)
{
  // Load fleet config file
  const auto fleet_config = YAML::LoadFile(config_file);
  // Check that config file is valid and contains all necessary nodes
  if (!fleet_config["rmf_fleet"])
  {
    std::cout
      << "RMF fleet configuration is not provided in the configuration file"
      << std::endl;
    return nullptr;
  }
  const YAML::Node rmf_fleet = fleet_config["rmf_fleet"];

  // Fleet name
  if (!rmf_fleet["name"])
  {
    std::cout << "Fleet name is not provided" << std::endl;
    return nullptr;
  }
  const std::string fleet_name = rmf_fleet["name"].as<std::string>();

  // Profile
  if (!rmf_fleet["profile"] || !rmf_fleet["profile"]["footprint"] ||
    !rmf_fleet["profile"]["vicinity"])
  {
    std::cout << "Fleet profile is not provided" << std::endl;
    return nullptr;
  }
  const YAML::Node profile = rmf_fleet["profile"];
  const double footprint_rad = profile["footprint"].as<double>();
  const double vicinity_rad = profile["vicinity"].as<double>();

  // Traits
  if (!rmf_fleet["limits"] || !rmf_fleet["limits"]["linear"] ||
    !rmf_fleet["limits"]["angular"])
  {
    std::cout << "Fleet traits are not provided" << std::endl;
    return nullptr;
  }
  const YAML::Node limits = rmf_fleet["limits"];
  const YAML::Node linear = limits["linear"];
  const double v_nom = linear[0].as<double>();
  const double a_nom = linear[1].as<double>();
  const YAML::Node angular = limits["angular"];
  const double w_nom = angular[0].as<double>();
  const double b_nom = angular[1].as<double>();

  // Reversibility
  bool reversible = false;
  if (!rmf_fleet["reversible"])
  {
    std::cout << "Fleet reversibility is not provided, default to False"
              << std::endl;
  }
  else
  {
    reversible = rmf_fleet["reversible"].as<bool>();
  }
  if (!reversible)
    std::cout << " ===== We have an irreversible robot" << std::endl;

  auto traits = std::make_shared<VehicleTraits>(VehicleTraits{
    {v_nom, a_nom},
    {w_nom, b_nom},
    rmf_traffic::Profile{
      rmf_traffic::geometry::make_final_convex<rmf_traffic::geometry::Circle>(
        footprint_rad),
      rmf_traffic::geometry::make_final_convex<rmf_traffic::geometry::Circle>(
        vicinity_rad)
    }
  });
  traits->get_differential()->set_reversible(reversible);

  // Graph
  const auto graph = parse_graph(nav_graph_path, *traits);

  // Set up parameters required for task planner
  // Battery system
  if (!rmf_fleet["battery_system"] || !rmf_fleet["battery_system"]["voltage"] ||
    !rmf_fleet["battery_system"]["capacity"] ||
    !rmf_fleet["battery_system"]["charging_current"])
  {
    std::cout << "Fleet battery system is not provided" << std::endl;
    return nullptr;
  }
  const YAML::Node battery = rmf_fleet["battery_system"];
  const double voltage = battery["voltage"].as<double>();
  const double capacity = battery["capacity"].as<double>();
  const double charging_current = battery["charging_current"].as<double>();

  const auto battery_system_optional = rmf_battery::agv::BatterySystem::make(
    voltage, capacity, charging_current);
  if (!battery_system_optional.has_value())
  {
    std::cout << "Invalid battery parameters" << std::endl;
    return nullptr;
  }
  const auto battery_system = std::make_shared<rmf_battery::agv::BatterySystem>(
    *battery_system_optional);

  // Mechanical system
  if (!rmf_fleet["mechanical_system"] ||
    !rmf_fleet["mechanical_system"]["mass"] ||
    !rmf_fleet["mechanical_system"]["moment_of_inertia"] ||
    !rmf_fleet["mechanical_system"]["friction_coefficient"])
  {
    std::cout << "Fleet mechanical system is not provided" << std::endl;
    return nullptr;
  }
  const YAML::Node mechanical = rmf_fleet["mechanical_system"];
  const double mass = mechanical["mass"].as<double>();
  const double moment_of_inertia = mechanical["moment_of_inertia"].as<double>();
  const double friction = mechanical["friction_coefficient"].as<double>();

  auto mechanical_system_optional = rmf_battery::agv::MechanicalSystem::make(
    mass, moment_of_inertia, friction);
  if (!mechanical_system_optional.has_value())
  {
    std::cout << "Invalid mechanical parameters" << std::endl;
    return nullptr;
  }
  rmf_battery::agv::MechanicalSystem& mechanical_system =
    *mechanical_system_optional;

  const auto motion_sink =
    std::make_shared<rmf_battery::agv::SimpleMotionPowerSink>(
    *battery_system, mechanical_system);

  // Ambient power system
  if (!rmf_fleet["ambient_system"] || !rmf_fleet["ambient_system"]["power"])
  {
    std::cout << "Fleet ambient system is not provided" << std::endl;
    return nullptr;
  }
  const YAML::Node ambient_system = rmf_fleet["ambient_system"];
  const double ambient_power_drain = ambient_system["power"].as<double>();
  auto ambient_power_system = rmf_battery::agv::PowerSystem::make(
    ambient_power_drain);
  if (!ambient_power_system)
  {
    std::cout << "Invalid values supplied for ambient power system"
              << std::endl;
    return nullptr;
  }
  const auto ambient_sink =
    std::make_shared<rmf_battery::agv::SimpleDevicePowerSink>(
    *battery_system, *ambient_power_system);

  // Tool power system
  if (!rmf_fleet["tool_system"] || !rmf_fleet["tool_system"]["power"])
  {
    std::cout << "Fleet tool system is not provided" << std::endl;
    return nullptr;
  }
  const YAML::Node tool_system = rmf_fleet["tool_system"];
  const double tool_power_drain = ambient_system["power"].as<double>();
  auto tool_power_system = rmf_battery::agv::PowerSystem::make(
    tool_power_drain);
  if (!tool_power_system)
  {
    std::cout << "Invalid values supplied for tool power system" << std::endl;
    return nullptr;
  }
  const auto tool_sink =
    std::make_shared<rmf_battery::agv::SimpleDevicePowerSink>(
    *battery_system, *tool_power_system);

  // Drain battery
  bool account_for_battery_drain = true;
  if (!rmf_fleet["account_for_battery_drain"])
  {
    std::cout << "Account for battery drain is not provided, default to True"
              << std::endl;
  }
  else
  {
    account_for_battery_drain =
      rmf_fleet["account_for_battery_drain"].as<bool>();
  }
  // Recharge threshold
  double recharge_threshold = 0.2;
  if (!rmf_fleet["recharge_threshold"])
  {
    std::cout
      << "Recharge threshold is not provided, default to 0.2" << std::endl;
  }
  else
  {
    recharge_threshold = rmf_fleet["recharge_threshold"].as<double>();
  }
  // Recharge state of charge
  double recharge_soc = 0.2;
  if (!rmf_fleet["recharge_soc"])
  {
    std::cout << "Recharge state of charge is not provided, default to 1.0"
              << std::endl;
  }
  else
  {
    recharge_soc = rmf_fleet["recharge_soc"].as<double>();
  }

  // Task capabilities
  if (!rmf_fleet["task_capabilities"] ||
    !rmf_fleet["task_capabilities"]["loop"] ||
    !rmf_fleet["task_capabilities"]["delivery"] ||
    !rmf_fleet["task_capabilities"]["clean"])
  {
    std::cout << "Fleet task capabilities are not provided" << std::endl;
    return nullptr;
  }
  const YAML::Node task_capabilities = rmf_fleet["task_capabilities"];
  std::unordered_map<std::string, ConsiderRequest> task_consideration;
  const auto parse_consideration = [&](
    const std::string& capability,
    const std::string& task)
    {
      if (const auto c = task_capabilities[capability])
      {
        if (c.as<bool>())
          task_consideration[task] = consider_all();
      }
    };

  parse_consideration("loop", "patrol");
  parse_consideration("patrol", "patrol");
  parse_consideration("clean", "clean");
  parse_consideration("delivery", "delivery");

  // Action considerations
  std::unordered_map<std::string, ConsiderRequest> action_consideration;
  if (task_capabilities["action"])
  {
    const auto actions =
      task_capabilities["action"].as<std::vector<std::string>>();
    for (const std::string& action : actions)
    {
      action_consideration[action] = consider_all();
    }
  }

  // Finishing tasks
  std::string finishing_request_string;
  if (!task_capabilities["finishing_request"])
  {
    std::cout
      << "Finishing request is not provided. The valid finishing requests "
      "are [charge, park, nothing]. The task planner will default to [nothing]."
      << std::endl;
  }
  else
  {
    finishing_request_string =
      task_capabilities["finishing_request"].as<std::string>();
  }
  rmf_task::ConstRequestFactoryPtr finishing_request;
  if (finishing_request_string == "charge")
  {
    finishing_request =
      std::make_shared<rmf_task::requests::ChargeBatteryFactory>();
    std::cout
      << "Fleet is configured to perform ChargeBattery as finishing request"
      << std::endl;
  }
  else if (finishing_request_string == "park")
  {
    finishing_request =
      std::make_shared<rmf_task::requests::ParkRobotFactory>();
    std::cout
      << "Fleet is configured to perform ParkRobot as finishing request"
      << std::endl;
  }
  else if (finishing_request_string == "nothing")
  {
    std::cout << "Fleet is not configured to perform any finishing request"
              << std::endl;
  }
  else
  {
    std::cout
      << "Provided finishing request " << finishing_request_string
      << "is unsupported. The valid finishing requests are"
      "[charge, park, nothing]. The task planner will default to [nothing].";
  }

  // Ignore rotations within path commands
  bool skip_rotation_commands = true;
  if (rmf_fleet["skip_rotation_commands"])
  {
    skip_rotation_commands = rmf_fleet["skip_rotation_commands"].as<bool>();
  }

  // Set the fleet state topic publish period
  double fleet_state_frequency = 2.0;
  if (!rmf_fleet["publish_fleet_state"])
  {
    std::cout
      << "Fleet state publish frequency is not provided, default to 2.0"
      << std::endl;
  }
  else
  {
    fleet_state_frequency = rmf_fleet["publish_fleet_state"].as<double>();
  }
  const double update_interval = 1.0/fleet_state_frequency;

  // Set the maximum delay
  double max_delay = 10.0;
  if (!rmf_fleet["max_delay"])
  {
    std::cout << "Maximum delay is not provided, default to 10.0" << std::endl;
  }
  else
  {
    max_delay = rmf_fleet["max_delay"].as<double>();
  }

  return std::make_shared<Configuration>(
    fleet_name,
    std::move(traits),
    std::make_shared<rmf_traffic::agv::Graph>(std::move(graph)),
    battery_system,
    motion_sink,
    ambient_sink,
    tool_sink,
    recharge_threshold,
    recharge_soc,
    account_for_battery_drain,
    task_consideration,
    action_consideration,
    finishing_request,
    skip_rotation_commands,
    server_uri,
    rmf_traffic::time::from_seconds(max_delay),
    rmf_traffic::time::from_seconds(update_interval));
}

//==============================================================================
const std::string& EasyFullControl::Configuration::fleet_name() const
{
  return _pimpl->fleet_name;
}

//==============================================================================
void EasyFullControl::Configuration::set_fleet_name(std::string value)
{
  _pimpl->fleet_name = std::move(value);
}

//==============================================================================
auto EasyFullControl::Configuration::vehicle_traits() const
-> const std::shared_ptr<const VehicleTraits>&
{
  return _pimpl->traits;
}

//==============================================================================
void EasyFullControl::Configuration::set_vehicle_traits(
  std::shared_ptr<const VehicleTraits> value)
{
  _pimpl->traits = std::move(value);
}

//==============================================================================
auto EasyFullControl::Configuration::graph() const
-> const std::shared_ptr<const Graph>&
{
  return _pimpl->graph;
}

//==============================================================================
void EasyFullControl::Configuration::set_graph(
  std::shared_ptr<const Graph> value)
{
  _pimpl->graph = std::move(value);
}

//==============================================================================
rmf_battery::agv::ConstBatterySystemPtr
EasyFullControl::Configuration::battery_system() const
{
  return _pimpl->battery_system;
}

//==============================================================================
void EasyFullControl::Configuration::set_battery_system(
  rmf_battery::agv::ConstBatterySystemPtr value)
{
  _pimpl->battery_system = std::move(value);
}

//==============================================================================
rmf_battery::ConstMotionPowerSinkPtr
EasyFullControl::Configuration::motion_sink() const
{
  return _pimpl->motion_sink;
}

//==============================================================================
void EasyFullControl::Configuration::set_motion_sink(
  rmf_battery::ConstMotionPowerSinkPtr value)
{
  _pimpl->motion_sink = std::move(value);
}

//==============================================================================
rmf_battery::ConstDevicePowerSinkPtr
EasyFullControl::Configuration::ambient_sink() const
{
  return _pimpl->ambient_sink;
}

//==============================================================================
void EasyFullControl::Configuration::set_ambient_sink(
  rmf_battery::ConstDevicePowerSinkPtr value)
{
  _pimpl->ambient_sink = std::move(value);
}

//==============================================================================
rmf_battery::ConstDevicePowerSinkPtr
EasyFullControl::Configuration::tool_sink() const
{
  return _pimpl->tool_sink;
}

//==============================================================================
void EasyFullControl::Configuration::set_tool_sink(
  rmf_battery::ConstDevicePowerSinkPtr value)
{
  _pimpl->tool_sink = std::move(value);
}

//==============================================================================
double EasyFullControl::Configuration::recharge_threshold() const
{
  return _pimpl->recharge_threshold;
}

//==============================================================================
void EasyFullControl::Configuration::set_recharge_threshold(double value)
{
  _pimpl->recharge_threshold = value;
}

//==============================================================================
double EasyFullControl::Configuration::recharge_soc() const
{
  return _pimpl->recharge_soc;
}

//==============================================================================
void EasyFullControl::Configuration::set_recharge_soc(double value)
{
  _pimpl->recharge_soc = value;
}

//==============================================================================
bool EasyFullControl::Configuration::account_for_battery_drain() const
{
  return _pimpl->account_for_battery_drain;
}

//==============================================================================
void EasyFullControl::Configuration::set_account_for_battery_drain(bool value)
{
  _pimpl->account_for_battery_drain = value;
}

//==============================================================================
const std::unordered_map<std::string, ConsiderRequest>&
EasyFullControl::Configuration::task_consideration() const
{
  return _pimpl->task_consideration;
}

//==============================================================================
std::unordered_map<std::string, ConsiderRequest>&
EasyFullControl::Configuration::task_consideration()
{
  return _pimpl->task_consideration;
}

//==============================================================================
const std::unordered_map<std::string, ConsiderRequest>&
EasyFullControl::Configuration::action_consideration() const
{
  return _pimpl->action_consideration;
}

//==============================================================================
std::unordered_map<std::string, ConsiderRequest>&
EasyFullControl::Configuration::action_consideration()
{
  return _pimpl->action_consideration;
}

//==============================================================================
rmf_task::ConstRequestFactoryPtr
EasyFullControl::Configuration::finishing_request() const
{
  return _pimpl->finishing_request;
}

//==============================================================================
void EasyFullControl::Configuration::set_finishing_request(
  rmf_task::ConstRequestFactoryPtr value)
{
  _pimpl->finishing_request = std::move(value);
}

//==============================================================================
bool EasyFullControl::Configuration::skip_rotation_commands() const
{
  return _pimpl->skip_rotation_commands;
}

//==============================================================================
void EasyFullControl::Configuration::set_skip_rotation_commands(bool value)
{
  _pimpl->skip_rotation_commands = value;
}

//==============================================================================
std::optional<std::string> EasyFullControl::Configuration::server_uri() const
{
  return _pimpl->server_uri;
}

//==============================================================================
void EasyFullControl::Configuration::set_server_uri(
  std::optional<std::string> value)
{
  _pimpl->server_uri = std::move(value);
}

//==============================================================================
rmf_traffic::Duration EasyFullControl::Configuration::max_delay() const
{
  return _pimpl->max_delay;
}

//==============================================================================
void EasyFullControl::Configuration::set_max_delay(rmf_traffic::Duration value)
{
  _pimpl->max_delay = value;
}

//==============================================================================
rmf_traffic::Duration EasyFullControl::Configuration::update_interval() const
{
  return _pimpl->update_interval;
}

//==============================================================================
void EasyFullControl::Configuration::set_update_interval(
  rmf_traffic::Duration value)
{
  _pimpl->update_interval = value;
}

//==============================================================================
class EasyFullControl::InitializeRobot::Implementation
{
public:
  std::string name;
  std::string charger_name;
  std::string map_name;
  Eigen::Vector3d location;
  double battery_soc;
};

//==============================================================================
EasyFullControl::InitializeRobot::InitializeRobot(
  const std::string& name,
  const std::string& charger_name,
  const std::string& map_name,
  Eigen::Vector3d location,
  double battery_soc)
: _pimpl(rmf_utils::make_impl<Implementation>(
      Implementation{
        std::move(name),
        std::move(charger_name),
        std::move(map_name),
        std::move(location),
        std::move(battery_soc)
      }))
{
  // Do nothing
}

//==============================================================================
const std::string& EasyFullControl::InitializeRobot::name() const
{
  return _pimpl->name;
}

//==============================================================================
const std::string& EasyFullControl::InitializeRobot::charger_name() const
{
  return _pimpl->charger_name;
}

//==============================================================================
const std::string& EasyFullControl::InitializeRobot::map_name() const
{
  return _pimpl->map_name;
}

//==============================================================================
const Eigen::Vector3d& EasyFullControl::InitializeRobot::location() const
{
  return _pimpl->location;
}

//==============================================================================
double EasyFullControl::InitializeRobot::battery_soc() const
{
  return _pimpl->battery_soc;
}

//==============================================================================
using EasyCommandHandlePtr = std::shared_ptr<EasyCommandHandle>;

//==============================================================================
std::shared_ptr<FleetUpdateHandle> EasyFullControl::more()
{
  return _pimpl->fleet_handle;
}

//==============================================================================
std::shared_ptr<const FleetUpdateHandle> EasyFullControl::more() const
{
  return _pimpl->fleet_handle;
}

//==============================================================================
auto EasyFullControl::add_robot(
  InitializeRobot initial_state,
  NavigationRequest handle_nav_request,
  StopRequest handle_stop,
  DockRequest handle_dock,
  ActionExecutor action_executor) -> std::shared_ptr<EasyRobotUpdateHandle>
{
  const auto& robot_name = initial_state.name();
  const auto node = _pimpl->node();
  RCLCPP_INFO(
    node->get_logger(),
    "Adding robot [%s] to the fleet.", robot_name.c_str()
  );

  const auto& fleet_impl =
    FleetUpdateHandle::Implementation::get(*_pimpl->fleet_handle);
  const auto& planner = *fleet_impl.planner;
  const auto& graph = planner->get_configuration().graph();
  const auto& traits = planner->get_configuration().vehicle_traits();
  const auto& fleet_name = _pimpl->fleet_handle->fleet_name();

  auto insertion = _pimpl->cmd_handles.insert({robot_name, nullptr});
  if (!insertion.second)
  {
    RCLCPP_WARN(
      node->get_logger(),
      "Robot [%s] was previously added to the fleet. Ignoring request...",
      robot_name.c_str()
    );
    return nullptr;
  }

  rmf_traffic::Time now = std::chrono::steady_clock::time_point(
    std::chrono::nanoseconds(node->now().nanoseconds()));

  auto starts = rmf_traffic::agv::compute_plan_starts(
    graph,
    initial_state.map_name(),
    initial_state.location(),
    std::move(now),
    _pimpl->nav_params->max_merge_waypoint_distance,
    _pimpl->nav_params->max_merge_lane_distance,
    _pimpl->nav_params->min_lane_length
  );

  if (starts.empty())
  {
    const auto& loc = initial_state.location();
    RCLCPP_ERROR(
      node->get_logger(),
      "Unable to compute a StartSet for robot [%s] being added to fleet [%s] "
      "using level_name [%s] and location [%.3f, %.3f, %.3f] specified in the "
      "InitializeRobot param. This can happen if the level_name in "
      "InitializeRobot does not match any of the map names in the navigation "
      "graph supplied or if the location reported in the InitializeRobot is "
      "far way from the navigation graph. This robot will not be added to the "
      "fleet.",
      robot_name.c_str(),
      fleet_name.c_str(),
      initial_state.map_name().c_str(),
      loc[0], loc[1], loc[2]);
    return nullptr;
  }

  if (handle_nav_request == nullptr ||
    handle_stop == nullptr ||
    handle_dock == nullptr)
  {
    RCLCPP_ERROR(
      node->get_logger(),
      "One or more required callbacks given to [EasyFullControl::add_robot] "
      "were null. The robot [%s] will not be added to fleet [%s].",
      robot_name.c_str(),
      _pimpl->fleet_handle->fleet_name().c_str());
    return nullptr;
  }

  const auto cmd_handle = std::make_shared<EasyCommandHandle>(
    _pimpl->nav_params,
    std::move(handle_nav_request),
    std::move(handle_stop),
    std::move(handle_dock));
  insertion.first->second = cmd_handle;

  auto worker = FleetUpdateHandle::Implementation::get(*_pimpl->fleet_handle).worker;
  auto easy_updater = EasyRobotUpdateHandle::Implementation::make(
    _pimpl->nav_params, worker);

  _pimpl->fleet_handle->add_robot(
    insertion.first->second,
    robot_name,
    traits.profile(),
    starts,
    [
      cmd_handle,
      easy_updater,
      node,
      robot_name = robot_name,
      fleet_name = fleet_name,
      action_executor = std::move(action_executor)
    ](const RobotUpdateHandlePtr& updater)
    {
      cmd_handle->w_context =
        RobotUpdateHandle::Implementation::get(*updater).get_context();

      EasyRobotUpdateHandle::Implementation::get(*easy_updater).worker.schedule(
        [
          easy_updater,
          node,
          handle = updater,
          robot_name,
          fleet_name,
          action_executor
        ](const auto&)
        {
          EasyRobotUpdateHandle::Implementation::get(*easy_updater)
            .updater->handle = handle;
          handle->set_action_executor(action_executor);

          RCLCPP_INFO(
            node->get_logger(),
            "Successfully added robot [%s] to the fleet [%s].",
            robot_name.c_str(),
            fleet_name.c_str());
        });
    });

  return easy_updater;
}

} // namespace agv
} // namespace rmf_fleet_adapter
