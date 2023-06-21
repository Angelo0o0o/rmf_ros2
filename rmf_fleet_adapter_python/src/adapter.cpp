#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/chrono.h>
#include <pybind11/eigen.h>
#include <pybind11/stl.h>
#include "pybind11_json/pybind11_json.hpp"
#include <memory>

#include "rmf_traffic_ros2/Time.hpp"
#include "rmf_fleet_adapter/agv/Adapter.hpp"
#include "rmf_fleet_adapter/agv/EasyFullControl.hpp"
#include "rmf_fleet_adapter/agv/test/MockAdapter.hpp"
#include "rmf_fleet_adapter_python/PyRobotCommandHandle.hpp"
#include <rmf_fleet_adapter/agv/Transformation.hpp>
#include <rmf_fleet_adapter/agv/Waypoint.hpp>
#include <rclcpp/rclcpp.hpp>

#include <rmf_battery/agv/BatterySystem.hpp>
#include <rmf_battery/agv/SimpleMotionPowerSink.hpp>
#include <rmf_battery/agv/SimpleDevicePowerSink.hpp>

#include <rmf_task/requests/ChargeBatteryFactory.hpp>
#include <rmf_task/requests/ParkRobotFactory.hpp>

#include <rmf_traffic/Time.hpp>
#include <rmf_traffic/agv/Planner.hpp>
#include <rmf_traffic/agv/VehicleTraits.hpp>

namespace py = pybind11;
namespace agv = rmf_fleet_adapter::agv;
namespace battery = rmf_battery::agv;

using TimePoint = std::chrono::time_point<std::chrono::system_clock,
    std::chrono::nanoseconds>;

/// Note: This ModifiedConsiderRequest is a minor alteration of ConsiderRequest
///       in FleetUpdateHandle. This is to replace the ref `confirm` arg with
///       a return value
using Confirmation = agv::FleetUpdateHandle::Confirmation;
using ConsiderRequest = agv::FleetUpdateHandle::ConsiderRequest;
using ModifiedConsiderRequest =
  std::function<Confirmation(const nlohmann::json &description)>;

std::unordered_map<std::string, ConsiderRequest> convert(
  const std::unordered_map<std::string, ModifiedConsiderRequest>& consideration)
{
  std::unordered_map<std::string, ConsiderRequest> output;
  for (const auto& element : consideration)
  {
    output[element.first] = [consider = element.second](
      const nlohmann::json& description, Confirmation& confirm)
    {
      confirm = consider(description);
    };
  }
}

using ActionExecution = agv::RobotUpdateHandle::ActionExecution;
using RobotInterruption = agv::RobotUpdateHandle::Interruption;
using IssueTicket = agv::RobotUpdateHandle::IssueTicket;
using Stubbornness = agv::RobotUpdateHandle::Unstable::Stubbornness;

void bind_types(py::module&);
void bind_graph(py::module&);
void bind_shapes(py::module&);
void bind_vehicletraits(py::module&);
void bind_plan(py::module&);
void bind_tests(py::module&);
void bind_nodes(py::module&);
void bind_battery(py::module&);
void bind_schedule(py::module&);

PYBIND11_MODULE(rmf_adapter, m) {
  bind_types(m);
  bind_graph(m);
  bind_shapes(m);
  bind_vehicletraits(m);
  bind_plan(m);
  bind_tests(m);
  bind_nodes(m);
  bind_battery(m);
  bind_schedule(m);

  // ROBOTCOMMAND HANDLE =====================================================
  // Abstract class
  py::class_<agv::RobotCommandHandle, PyRobotCommandHandle,
    std::shared_ptr<agv::RobotCommandHandle>>(
    m, "RobotCommandHandle", py::dynamic_attr())
  .def(py::init<>())
  .def("follow_new_path", &agv::RobotCommandHandle::follow_new_path)
  .def("stop", &agv::RobotCommandHandle::stop)
  .def("dock", &agv::RobotCommandHandle::dock);

  m.def("consider_all", []() -> ModifiedConsiderRequest {
    return [](const nlohmann::json&) -> Confirmation
    {
      Confirmation confirm;
      confirm.accept();
      return confirm;
    };
  });

  // ROBOTUPDATE HANDLE ======================================================
  py::class_<agv::RobotUpdateHandle,
    std::shared_ptr<agv::RobotUpdateHandle>>(
    m, "RobotUpdateHandle")
  // Private constructor: Only to be constructed via FleetUpdateHandle!
  .def("interrupted", &agv::RobotUpdateHandle::replan)
  .def("replan", &agv::RobotUpdateHandle::replan)
  .def("update_current_waypoint",
    py::overload_cast<std::size_t, double>(
      &agv::RobotUpdateHandle::update_position),
    py::arg("waypoint"),
    py::arg("orientation"))
  .def("update_current_lanes",
    py::overload_cast<const Eigen::Vector3d&,
    const std::vector<std::size_t>&>(
      &agv::RobotUpdateHandle::update_position),
    py::arg("position"),
    py::arg("lanes"))
  .def("update_off_grid_position",
    py::overload_cast<const Eigen::Vector3d&,
    std::size_t>(
      &agv::RobotUpdateHandle::update_position),
    py::arg("position"),
    py::arg("target_waypoint"))
  .def("update_lost_position",
    py::overload_cast<const std::string&,
    const Eigen::Vector3d&,
    const double,
    const double,
    const double>(
      &agv::RobotUpdateHandle::update_position),
    py::arg("map_name"),
    py::arg("position"),
    py::arg("max_merge_waypoint_distance") = 0.1,
    py::arg("max_merge_lane_distance") = 1.0,
    py::arg("min_lane_length") = 1e-8)
  .def("update_position",
    py::overload_cast<rmf_traffic::agv::Plan::StartSet>(
      &agv::RobotUpdateHandle::update_position),
    py::arg("start_set"))
  .def("set_charger_waypoint", &agv::RobotUpdateHandle::set_charger_waypoint,
    py::arg("charger_wp"))
  .def("update_battery_soc", &agv::RobotUpdateHandle::update_battery_soc,
    py::arg("battery_soc"))
  .def("override_status", &agv::RobotUpdateHandle::override_status,
    py::arg("new_status"))
  .def_property("maximum_delay",
    py::overload_cast<>(
      &agv::RobotUpdateHandle::maximum_delay, py::const_),
    [&](agv::RobotUpdateHandle& self)
    {
      return self.maximum_delay();
    })
  .def("set_infinite_delay",
    [&](agv::RobotUpdateHandle& self)
    {
      self.maximum_delay(rmf_utils::nullopt);
    })
  .def("set_maximum_delay",
    [&](agv::RobotUpdateHandle& self,
    double seconds)
    {
      const auto duration = rmf_traffic::time::from_seconds(seconds);
      self.maximum_delay(duration);
    },
    py::arg("seconds"))
  .def("unstable_is_commissioned",
    [&](const agv::RobotUpdateHandle& self)
    {
      return self.unstable().is_commissioned();
    },
    "Check if the robot is currently allowed to accept any new tasks")
  .def("unstable_decommission",
    [&](agv::RobotUpdateHandle& self)
    {
      return self.unstable().decommission();
    },
    "Stop this robot from accepting any new tasks. Use recommission to resume")
  .def("unstable_recommission",
    [&](agv::RobotUpdateHandle& self)
    {
      return self.unstable().recommission();
    },
    "Allow this robot to resume accepting new tasks if it was ever "
    "decommissioned in the past")
  .def("get_unstable_participant",
    [&](agv::RobotUpdateHandle& self)
    {
      return self.unstable().get_participant();
    },
    py::return_value_policy::reference_internal,
    "Experimental API to access the schedule participant")
  .def("unstable_get_participant",
    [&](agv::RobotUpdateHandle& self)
    {
      // This is the same as get_unstable_participant, which was the original
      // function signature for this binding. Since "unstable" describes the
      // API and does not describe the participant, it should be at the front
      // of the function name, not attached to "participant". But too many
      // downstream packages are using get_unstable_participant, so we cannot
      // simply remove support for it.
      return self.unstable().get_participant();
    },
    py::return_value_policy::reference_internal,
    "Experimental API to access the schedule participant")
  .def("unstable_change_participant_profile",
    [&](agv::RobotUpdateHandle& self,
    double footprint_radius,
    double vicinity_radius)
    {
      self.unstable().change_participant_profile(
        footprint_radius, vicinity_radius);
    },
    py::arg("footprint_radius"),
    py::arg("vicinity_radius"))
  .def("unstable_declare_holding",
    [&](agv::RobotUpdateHandle& self,
    std::string on_map,
    Eigen::Vector3d at_position,
    double for_duration)
    {
      self.unstable().declare_holding(
        std::move(on_map),
        at_position,
        rmf_traffic::time::from_seconds(for_duration));
    },
    py::arg("on_map"),
    py::arg("at_position"),
    py::arg("for_duration"))
  .def("unstable_current_plan_id",
    [&](const agv::RobotUpdateHandle& self)
    {
      return self.unstable().current_plan_id();
    })
  .def("unstable_be_stubborn",
    [&](agv::RobotUpdateHandle& self)
    {
      return self.unstable().be_stubborn();
    })
  .def("set_action_executor",
    &agv::RobotUpdateHandle::set_action_executor,
    py::arg("action_executor"))
  .def("submit_direct_request",
    &agv::RobotUpdateHandle::submit_direct_request,
    py::arg("task_request"),
    py::arg("request_id"),
    py::arg("receive_response"))
  .def("interrupt",
    &agv::RobotUpdateHandle::interrupt,
    py::arg("labels"),
    py::arg("robot_is_interrupted"))
  .def("cancel_task",
    &agv::RobotUpdateHandle::cancel_task,
    py::arg("task_id"),
    py::arg("labels"),
    py::arg("on_cancellation"))
  .def("kill_task",
    &agv::RobotUpdateHandle::kill_task,
    py::arg("task_id"),
    py::arg("labels"),
    py::arg("on_kill"))
  .def("create_issue",
    &agv::RobotUpdateHandle::create_issue,
    py::arg("tier"),
    py::arg("category"),
    py::arg("detail"))
  .def("log_info",
    &agv::RobotUpdateHandle::log_info,
    py::arg("text"))
  .def("log_warning",
    &agv::RobotUpdateHandle::log_warning,
    py::arg("text"))
  .def("log_error",
    &agv::RobotUpdateHandle::log_error,
    py::arg("text"))
  .def("enable_responsive_wait",
    &agv::RobotUpdateHandle::enable_responsive_wait,
    py::arg("value"));

  // ACTION EXECUTOR   =======================================================
  auto m_robot_update_handle = m.def_submodule("robot_update_handle");

  py::class_<ActionExecution>(
    m_robot_update_handle, "ActionExecution")
  .def("update_remaining_time",
    &ActionExecution::update_remaining_time,
    py::arg("remaining_time_estimate"))
  .def("underway", &ActionExecution::underway, py::arg("text"))
  .def("error", &ActionExecution::error, py::arg("text"))
  .def("delayed", &ActionExecution::delayed, py::arg("text"))
  .def("blocked", &ActionExecution::blocked, py::arg("text"))
  .def("replan", &ActionExecution::replan)
  .def("override_schedule", &ActionExecution::override_schedule,
    py::arg("map_name"), py::arg("trajectory"))
  .def("finished", &ActionExecution::finished)
  .def("okay", &ActionExecution::okay);

  // ROBOT INTERRUPTION   ====================================================
  py::class_<RobotInterruption>(
    m_robot_update_handle, "RobotInterruption")
  .def("resume",
    &RobotInterruption::resume,
    py::arg("labels"));

  // ISSUE TICKET   ==========================================================
  py::class_<IssueTicket>(
    m_robot_update_handle, "IssueTicket")
  .def("resolve",
    &IssueTicket::resolve,
    py::arg("msg"));

  // Tier   ==================================================================
  py::enum_<agv::RobotUpdateHandle::Tier>(
    m_robot_update_handle, "Tier")
    .value("Info", agv::RobotUpdateHandle::Tier::Info)
    .value("Warning", agv::RobotUpdateHandle::Tier::Warning)
    .value("Error", agv::RobotUpdateHandle::Tier::Error);

  // Stubbornness ============================================================
  py::class_<Stubbornness>(
    m_robot_update_handle, "Stubbornness")
  .def("release",
    &Stubbornness::release);

  // FLEETUPDATE HANDLE ======================================================
  py::class_<agv::FleetUpdateHandle,
    std::shared_ptr<agv::FleetUpdateHandle>>(
    m, "FleetUpdateHandle")
  // NOTE(CH3): Might have to publicise this constructor if required
  // Otherwise, it's constructable via agv::TestScenario
  // .def(py::init<>())  // Private constructor!

  // Also, intentionally missing accept_delivery_requests
  .def("add_robot", &agv::FleetUpdateHandle::add_robot,
    // NOTE(CH3): Saved for posterity...
    /* py::overload_cast<std::shared_ptr<agv::RobotCommandHandle>,
                      const std::string&,
                      const rmf_traffic::Profile&,
                      rmf_traffic::agv::Plan::StartSet,
                      std::function<void(
                        std::shared_ptr<RobotUpdateHandle> handle)>
                        >(&agv::FleetUpdateHandle::add_robot), */
    py::arg("command"),
    py::arg("name"),
    py::arg("profile"),
    py::arg("start"),
    py::arg("handle_cb"))
  .def("close_lanes",
    &agv::FleetUpdateHandle::close_lanes,
    py::arg("lane_indices"))
  .def("open_lanes",
    &agv::FleetUpdateHandle::open_lanes,
    py::arg("lane_indices"))
  .def("set_task_planner_params",
    [&](agv::FleetUpdateHandle& self,
    battery::BatterySystem& b_sys,
    battery::SimpleMotionPowerSink& m_sink,
    battery::SimpleDevicePowerSink& a_sink,
    battery::SimpleDevicePowerSink& t_sink,
    double recharge_threshold,
    double recharge_soc,
    bool account_for_battery_drain,
    const std::string& finishing_request_string = "nothing")
    {
      // Supported finishing_request_string: [charge, park, nothing]
      rmf_task::ConstRequestFactoryPtr finishing_request;
      if (finishing_request_string == "charge")
      {
        finishing_request =
        std::make_shared<rmf_task::requests::ChargeBatteryFactory>();
      }
      else if (finishing_request_string == "park")
      {
        finishing_request =
        std::make_shared<rmf_task::requests::ParkRobotFactory>();
      }
      else
      {
        finishing_request = nullptr;
      }

      return self.set_task_planner_params(
        std::make_shared<battery::BatterySystem>(b_sys),
        std::make_shared<battery::SimpleMotionPowerSink>(m_sink),
        std::make_shared<battery::SimpleDevicePowerSink>(a_sink),
        std::make_shared<battery::SimpleDevicePowerSink>(t_sink),
        recharge_threshold,
        recharge_soc,
        account_for_battery_drain,
        finishing_request);
    },
    py::arg("battery_system"),
    py::arg("motion_sink"),
    py::arg("ambient_sink"),
    py::arg("tool_sink"),
    py::arg("recharge_threshold"),
    py::arg("recharge_soc"),
    py::arg("account_for_battery_drain"),
    py::arg("finishing_request_string") = "nothing")
  .def("accept_delivery_requests",
    &agv::FleetUpdateHandle::accept_delivery_requests,
    "NOTE: deprecated, use consider_delivery_requests() instead")
  .def("accept_task_requests",
    &agv::FleetUpdateHandle::accept_task_requests,
    py::arg("check"),
    "NOTE: deprecated, use the consider_..._requests functions instead")
  .def_property("default_maximum_delay",
    py::overload_cast<>(
      &agv::FleetUpdateHandle::default_maximum_delay, py::const_),
    [&](agv::FleetUpdateHandle& self)
    {
      return self.default_maximum_delay();
    })
  .def("fleet_state_publish_period",
    &agv::FleetUpdateHandle::fleet_state_publish_period,
    py::arg("value"),
    "NOTE, deprecated, Use fleet_state_topic_publish_period instead")
  .def("fleet_state_topic_publish_period",
    &agv::FleetUpdateHandle::fleet_state_topic_publish_period,
    py::arg("value"),
    "Specify a period for how often the fleet state is updated in the\
     database and to the API server, default value is 1s, passing None\
     will disable the updating")
  .def("fleet_state_update_period",
    &agv::FleetUpdateHandle::fleet_state_update_period,
    py::arg("value"),
    "Specify a period for how often the fleet state message is published for\
     this fleet. Passing in None will disable the fleet state message\
     publishing. The default value is 1s")
  .def("set_update_listener",
    &agv::FleetUpdateHandle::set_update_listener,
    py::arg("listener"),
    "Provide a callback that will receive fleet state and task updates.")
  .def("consider_delivery_requests",
     [&](agv::FleetUpdateHandle& self,
         ModifiedConsiderRequest consider_pickup,
         ModifiedConsiderRequest consider_dropoff)
    {
      self.consider_delivery_requests(
          [consider_pickup = std::move(consider_pickup)](
            const nlohmann::json &description, Confirmation &confirm)
          {
            confirm = consider_pickup(description); // confirm is returned by user
          },
          [consider_dropoff = std::move(consider_dropoff)](
            const nlohmann::json &description, Confirmation &confirm)
          {
            confirm = consider_dropoff(description); // confirm is returned by user
          }
        );
    },
    py::arg("consider_pickup"),
    py::arg("consider_dropoff"))
  .def("consider_cleaning_requests",
     [&](agv::FleetUpdateHandle& self,
         ModifiedConsiderRequest consider)
    {
      self.consider_cleaning_requests(
          [consider = std::move(consider)](
            const nlohmann::json &description, Confirmation &confirm)
          {
            confirm = consider(description); // confirm is returned by user
          }
        );
    },
    py::arg("consider"))
  .def("consider_patrol_requests",
     [&](agv::FleetUpdateHandle& self,
         ModifiedConsiderRequest consider)
    {
      self.consider_patrol_requests(
          [consider = std::move(consider)](
            const nlohmann::json &description, Confirmation &confirm)
          {
            confirm = consider(description); // confirm is returned by user
          }
        );
    },
    py::arg("consider"))
  .def("consider_composed_requests",
     [&](agv::FleetUpdateHandle& self,
         ModifiedConsiderRequest consider)
    {
      self.consider_composed_requests(
          [consider = std::move(consider)](
            const nlohmann::json &description, Confirmation &confirm)
          {
            confirm = consider(description); // confirm is returned by user
          }
        );
    },
    py::arg("consider"))
  .def("add_performable_action",
     [&](agv::FleetUpdateHandle& self,
         const std::string& category,
         ModifiedConsiderRequest consider)
    {
      self.add_performable_action(
          category,
          [consider = std::move(consider)](
            const nlohmann::json &description, Confirmation &confirm)
          {
            confirm = consider(description); // confirm is returned by user
          }
        );
    },
    py::arg("category"),
    py::arg("consider"));

  // TASK REQUEST CONFIRMATION ===============================================
  auto m_fleet_update_handle = m.def_submodule("fleet_update_handle");

  py::class_<Confirmation>(
    m_fleet_update_handle, "Confirmation")
  .def(py::init<>())
  .def("accept",
    &Confirmation::accept, py::return_value_policy::reference_internal)
  .def("is_accepted", &Confirmation::is_accepted)
  .def("set_errors",
    [&](Confirmation& self, std::vector<std::string> error_messages)
    {
      self.errors(error_messages);
    })
  .def("add_errors",
    &Confirmation::add_errors,
    py::arg("value"),
    py::return_value_policy::reference_internal)
  .def_property("errors",
    py::overload_cast<>(
      &Confirmation::errors, py::const_),\
      [&](Confirmation& self)
    {
      return self.errors();
    });

  // EASY TRAFFIC LIGHT HANDLE ===============================================
  py::class_<agv::Waypoint>(m, "Waypoint")
  .def(py::init<std::string,
    Eigen::Vector3d,
    rmf_traffic::Duration,
    bool>(),
    py::arg("map_name"),
    py::arg("position"),
    py::arg("mandatory_delay"),
    py::arg("yield"))
  .def_property("map_name",
    py::overload_cast<>(&agv::Waypoint::map_name, py::const_),
    py::overload_cast<std::string>(&agv::Waypoint::map_name))
  .def_property("position",
    py::overload_cast<>(&agv::Waypoint::position, py::const_),
    py::overload_cast<Eigen::Vector3d>(&agv::Waypoint::position))
  .def_property("mandatory_delay",
    py::overload_cast<>(&agv::Waypoint::mandatory_delay, py::const_),
    py::overload_cast<rmf_traffic::Duration>(
      &agv::Waypoint::mandatory_delay))
  .def_property("yield",
    py::overload_cast<>(&agv::Waypoint::yield, py::const_),
    py::overload_cast<bool>(&agv::Waypoint::yield));

  // EASY TRAFFIC LIGHT HANDLE ===============================================
  py::class_<agv::EasyTrafficLight,
    std::shared_ptr<agv::EasyTrafficLight>>(
    m, "EasyTrafficLight", py::dynamic_attr())
  .def("follow_new_path",
    py::overload_cast<const std::vector<agv::Waypoint>&>(
      &agv::EasyTrafficLight::follow_new_path),
    py::arg("waypoint"))
  .def("moving_from",
    py::overload_cast<std::size_t, Eigen::Vector3d>(
      &agv::EasyTrafficLight::moving_from),
    py::arg("checkpoint"),
    py::arg("location"))
  .def("waiting_at",
    py::overload_cast<std::size_t>(
      &agv::EasyTrafficLight::waiting_at),
    py::arg("checkpoint"))
  .def("waiting_after",
    py::overload_cast<std::size_t, Eigen::Vector3d>(
      &agv::EasyTrafficLight::waiting_after),
    py::arg("checkpoint"),
    py::arg("location"))
  .def("last_reached", &agv::EasyTrafficLight::last_reached)
  .def("update_idle_location",
    py::overload_cast<std::string, Eigen::Vector3d>(
      &agv::EasyTrafficLight::update_idle_location),
    py::arg("map_name"),
    py::arg("position"));

  // prefix traffic light
  auto m_easy_traffic_light = m.def_submodule("easy_traffic_light");

  py::enum_<agv::EasyTrafficLight::MovingInstruction>(
    m_easy_traffic_light, "MovingInstruction")
  .value("MovingError",
    agv::EasyTrafficLight::MovingInstruction::MovingError)
  .value("ContinueAtNextCheckpoint",
    agv::EasyTrafficLight::MovingInstruction::ContinueAtNextCheckpoint)
  .value("WaitAtNextCheckpoint",
    agv::EasyTrafficLight::MovingInstruction::WaitAtNextCheckpoint)
  .value("PauseImmediately",
    agv::EasyTrafficLight::MovingInstruction::PauseImmediately);

  py::enum_<agv::EasyTrafficLight::WaitingInstruction>(
    m_easy_traffic_light, "WaitingInstruction")
  .value("WaitingError",
    agv::EasyTrafficLight::WaitingInstruction::WaitingError)
  .value("Resume",
    agv::EasyTrafficLight::WaitingInstruction::Resume)
  .value("Wait",
    agv::EasyTrafficLight::WaitingInstruction::Wait);

  // ADAPTER =================================================================
  // Light wrappers
  py::class_<rclcpp::NodeOptions>(m, "NodeOptions")
  .def(py::init<>());

  py::class_<rclcpp::Node, std::shared_ptr<rclcpp::Node>>(m, "Node")
  .def("now", [](rclcpp::Node& self)
    {
      return rmf_traffic_ros2::convert(self.now());
    })
  .def("use_sim_time", [](rclcpp::Node& self)
    {
      rclcpp::Parameter param("use_sim_time", true);
      self.set_parameter(param);
    });

  // Python rclcpp init and spin call
  m.def("init_rclcpp", []() { rclcpp::init(0, nullptr); });
  m.def("spin_rclcpp", [](rclcpp::Node::SharedPtr node_pt)
    {
      rclcpp::spin(node_pt);
    });
  m.def("spin_some_rclcpp", [](rclcpp::Node::SharedPtr node_pt)
    {
      rclcpp::spin_some(node_pt);
    });

  py::class_<agv::Adapter, std::shared_ptr<agv::Adapter>>(m, "Adapter")
  // .def(py::init<>())  // Private constructor
  .def_static("make", &agv::Adapter::make,
    py::arg("node_name"),
    py::arg("node_options") = rclcpp::NodeOptions(),
    py::arg("wait_time") = rmf_utils::optional<rmf_traffic::Duration>(
      rmf_utils::nullopt))
  .def("add_easy_fleet", &agv::Adapter::add_easy_fleet,
    py::arg("configuration"))
  .def("add_fleet", &agv::Adapter::add_fleet,
    py::arg("fleet_name"),
    py::arg("traits"),
    py::arg("navigation_graph"),
    py::arg("server_uri") = std::nullopt)
  .def("add_easy_traffic_light", &agv::Adapter::add_easy_traffic_light,
    py::arg("handle_callback"),
    py::arg("fleet_name"),
    py::arg("robot_name"),
    py::arg("traits"),
    py::arg("pause_callback"),
    py::arg("resume_callback"),
    py::arg("blocker_callback") = nullptr)
  .def_property_readonly("node",
    py::overload_cast<>(&agv::Adapter::node))
  .def("start", &agv::Adapter::start)
  .def("stop", &agv::Adapter::stop)
  .def("now", [](agv::Adapter& self)
    {
      return TimePoint(rmf_traffic_ros2::convert(self.node()->now())
      .time_since_epoch());
    });

  py::class_<agv::test::MockAdapter,
    std::shared_ptr<agv::test::MockAdapter>>(m, "MockAdapter")
  .def(py::init<const std::string&,
    const rclcpp::NodeOptions&>(),
    py::arg("node_name"),
    py::arg("node_options") = rclcpp::NodeOptions())
  .def("add_fleet", &agv::test::MockAdapter::add_fleet,
    py::arg("fleet_name"),
    py::arg("traits"),
    py::arg("navigation_graph"),
    py::arg("server_uri") = std::nullopt)
  .def_property_readonly("node",
    py::overload_cast<>(
      &agv::test::MockAdapter::node))
   /// Note: Exposed dispatch_task() for testing
  .def("dispatch_task",
    &agv::test::MockAdapter::dispatch_task,
    py::arg("task_id"),
    py::arg("request"))
  .def("start", &agv::test::MockAdapter::start)
  .def("stop", &agv::test::MockAdapter::stop)
  .def("now", [&](agv::test::MockAdapter& self)
    {
      return TimePoint(rmf_traffic_ros2::convert(self.node()->now())
      .time_since_epoch());
    });

  // EASY FULL CONTROL HANDLE ===============================================
  py::class_<agv::EasyFullControl, std::shared_ptr<agv::EasyFullControl>>(
    m, "EasyFullControl")
  .def("add_robot", [](agv::EasyFullControl& self,
    agv::EasyFullControl::InitializeRobot initial_state,
    agv::EasyFullControl::NavigationRequest handle_nav_request,
    agv::EasyFullControl::StopRequest handle_stop,
    agv::EasyFullControl::DockRequest handle_dock,
    agv::EasyFullControl::ActionExecutor action_executor)
    {
      return self.add_robot(initial_state, handle_nav_request,
        handle_stop, handle_dock, action_executor);
    },
    py::arg("initial_state"),
    py::arg("handle_nav_request"),
    py::arg("handle_stop"),
    py::arg("handle_dock"),
    py::arg("action_executor"))
  .def("more", [](agv::EasyFullControl& self)
    {
      return self.more();
    });
  // EASY FULL CONTROL CONFIGURATION ===============================================
  auto m_easy_full_control = m.def_submodule("easy_full_control");

  py::class_<agv::EasyFullControl::Configuration>(m_easy_full_control, "Configuration")
  .def(py::init([]( // Lambda function to convert reference to shared ptr
        std::string& fleet_name,
        rmf_traffic::agv::VehicleTraits& traits,
        rmf_traffic::agv::Graph& graph,
        battery::BatterySystem& battery_system,
        battery::SimpleMotionPowerSink& motion_sink,
        battery::SimpleDevicePowerSink& ambient_sink,
        battery::SimpleDevicePowerSink& tool_sink,
        double recharge_threshold,
        double recharge_soc,
        bool account_for_battery_drain,
        std::unordered_map<std::string, ModifiedConsiderRequest> task_consideration,
        std::unordered_map<std::string, ModifiedConsiderRequest> action_consideration,
        std::string& finishing_request_string,
        bool skip_rotation_commands,
        std::optional<std::string> server_uri,
        rmf_traffic::Duration max_delay,
        rmf_traffic::Duration update_interval)
        {
          rmf_task::ConstRequestFactoryPtr finishing_request;
          if (finishing_request_string == "charge")
          {
            finishing_request =
            std::make_shared<rmf_task::requests::ChargeBatteryFactory>();
          }
          else if (finishing_request_string == "park")
          {
            finishing_request =
            std::make_shared<rmf_task::requests::ParkRobotFactory>();
          }
          else
          {
            finishing_request = nullptr;
          }
          return agv::EasyFullControl::Configuration(
              fleet_name,
              traits,
              graph,
              std::make_shared<battery::BatterySystem>(battery_system),
              std::make_shared<battery::SimpleMotionPowerSink>(motion_sink),
              std::make_shared<battery::SimpleDevicePowerSink>(ambient_sink),
              std::make_shared<battery::SimpleDevicePowerSink>(tool_sink),
              recharge_threshold,
              recharge_soc,
              account_for_battery_drain,
              convert(task_consideration),
              convert(action_consideration),
              finishing_request,
              skip_rotation_commands,
              server_uri,
              max_delay,
              update_interval);
        }
        ),
    py::arg("fleet_name"),
    py::arg("traits"),
    py::arg("graph"),
    py::arg("battery_system"),
    py::arg("motion_sink"),
    py::arg("ambient_sink"),
    py::arg("tool_sink"),
    py::arg("recharge_threshold"),
    py::arg("recharge_soc"),
    py::arg("account_for_battery_drain"),
    py::arg("task_categories"),
    py::arg("action_categories"),
    py::arg("finishing_request") = "nothing",
    py::arg("skip_rotation_commands") = true,
    py::arg("server_uri") = std::nullopt,
    py::arg("max_delay") = rmf_traffic::time::from_seconds(10.0),
    py::arg("update_interval") = rmf_traffic::time::from_seconds(0.5))
  .def_static("from_config_files", &agv::EasyFullControl::Configuration::from_config_files,
    py::arg("config_file"),
    py::arg("nav_graph_path"),
    py::arg("server_uri"))
  .def("fleet_name", &agv::EasyFullControl::Configuration::fleet_name)
  .def("vehicle_traits", &agv::EasyFullControl::Configuration::vehicle_traits)
  .def("graph", &agv::EasyFullControl::Configuration::graph)
  .def("battery_system", &agv::EasyFullControl::Configuration::battery_system)
  .def("motion_sink", &agv::EasyFullControl::Configuration::motion_sink)
  .def("ambient_sink", &agv::EasyFullControl::Configuration::ambient_sink)
  .def("tool_sink", &agv::EasyFullControl::Configuration::tool_sink)
  .def("recharge_threshold", &agv::EasyFullControl::Configuration::recharge_threshold)
  .def("recharge_soc", &agv::EasyFullControl::Configuration::recharge_soc)
  .def("account_for_battery_drain", &agv::EasyFullControl::Configuration::account_for_battery_drain)
  .def("finishing_request", &agv::EasyFullControl::Configuration::finishing_request)
  .def("server_uri", &agv::EasyFullControl::Configuration::server_uri)
  .def("max_delay", &agv::EasyFullControl::Configuration::max_delay)
  .def("update_interval", &agv::EasyFullControl::Configuration::update_interval);
      /*
  .def_property("fleet_name",
    py::overload_cast<>(&agv::Configuration::fleet_name, py::const_),
    py::overload_cast<std::string>(&agv::Configuration::fleet_name))
  .def_property("traits",
    py::overload_cast<>(&agv::Configuration::traits, py::const_),
    py::overload_cast<rmf_traffic::agv::VechileTraits>(&agv::Configuration::vehicle_traits))
  .def_property("graph",
    py::overload_cast<>(&agv::Configuration::graph, py::const_),
    py::overload_cast<rmf_traffic::Duration>(
      &agv::Configuration::mandatory_delay))
  .def_property("yield",
    py::overload_cast<>(&agv::Configuration::yield, py::const_),
    py::overload_cast<bool>(&agv::Configuration::yield));
  .def_property("map_name",
    py::overload_cast<>(&agv::Configuration::map_name, py::const_),
    py::overload_cast<std::string>(&agv::Configuration::map_name))
  .def_property("position",
    py::overload_cast<>(&agv::Configuration::position, py::const_),
    py::overload_cast<Eigen::Vector3d>(&agv::Configuration::position))
  .def_property("mandatory_delay",
    py::overload_cast<>(&agv::Configuration::mandatory_delay, py::const_),
    py::overload_cast<rmf_traffic::Duration>(
      &agv::Configuration::mandatory_delay))
  .def_property("yield",
    py::overload_cast<>(&agv::Configuration::yield, py::const_),
    py::overload_cast<bool>(&agv::Configuration::yield));
    */

  // EASY FULL CONTROL RobotState ===============================================
  py::class_<agv::EasyFullControl::InitializeRobot>(m_easy_full_control, "InitializeRobot")
  .def(py::init<
      const std::string&,
      const std::string&,
      const std::string&,
      Eigen::Vector3d,
      double>(),
    py::arg("name"),
    py::arg("charger_name"),
    py::arg("map_name"),
    py::arg("location"),
    py::arg("battery_soc"))
  .def_property_readonly("name", &agv::EasyFullControl::InitializeRobot::name)
  .def_property_readonly("charger_name", &agv::EasyFullControl::InitializeRobot::charger_name)
  .def_property_readonly("map_name", &agv::EasyFullControl::InitializeRobot::map_name)
  .def_property_readonly("location", &agv::EasyFullControl::InitializeRobot::location)
  .def_property_readonly("battery_soc", &agv::EasyFullControl::InitializeRobot::battery_soc);

  // Transformation =============================================================
  py::class_<agv::Transformation>(m, "Transformation")
  .def(py::init<double,
    double,
    Eigen::Vector2d>(),
    py::arg("rotation"),
    py::arg("scale"),
    py::arg("translation"))
  .def("rotation", &agv::Transformation::rotation)
  .def("scale", &agv::Transformation::scale)
  .def("translation", &agv::Transformation::translation);

  m.def("transform", [](
    const agv::Transformation& transformation,
    const Eigen::Vector3d& pose)
  {
    return agv::transform(transformation, pose);
  });
}
