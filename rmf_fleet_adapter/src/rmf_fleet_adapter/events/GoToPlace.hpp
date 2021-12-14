/*
 * Copyright (C) 2021 Open Source Robotics Foundation
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

#ifndef SRC__RMF_FLEET_ADAPTER__EVENTS__GOTOPLACE_HPP
#define SRC__RMF_FLEET_ADAPTER__EVENTS__GOTOPLACE_HPP

#include "../agv/RobotContext.hpp"
#include "../Negotiator.hpp"

#include "../services/FindPath.hpp"

#include <rmf_task_sequence/Event.hpp>
#include <rmf_task_sequence/events/GoToPlace.hpp>
#include <rmf_task/events/SimpleEventState.hpp>

#include <rmf_task_sequence/events/Bundle.hpp>

namespace rmf_fleet_adapter {
namespace events {

//==============================================================================
class GoToPlace : public rmf_task_sequence::Event
{
public:

  class Standby : public rmf_task_sequence::Event::Standby
  {
  public:

    static std::shared_ptr<Standby> make(
      const AssignIDPtr& id,
      const std::function<rmf_task::State()>& get_state,
      const rmf_task::ConstParametersPtr& parameters,
      const rmf_task_sequence::events::GoToPlace::Description& description,
      std::function<void()> update,
      std::optional<rmf_traffic::Duration> tail_period = std::nullopt);

    ConstStatePtr state() const final;

    rmf_traffic::Duration duration_estimate() const final;

    ActivePtr begin(
      std::function<void()> checkpoint,
      std::function<void()> finished) final;

  private:
    AssignIDPtr _assign_id;
    agv::RobotContextPtr _context;
    rmf_traffic::agv::Plan::Goal _goal;
    rmf_traffic::Duration _time_estimate;
    std::optional<rmf_traffic::Duration> _tail_period;
    std::function<void()> _update;
    rmf_task::events::SimpleEventStatePtr _state;
    ActivePtr _active = nullptr;
  };

  class Active
    : public rmf_task_sequence::Event::Active,
      public std::enable_shared_from_this<Active>
  {
  public:

    static std::shared_ptr<Active> make(
      const AssignIDPtr& id,
      agv::RobotContextPtr context,
      rmf_traffic::agv::Plan::Goal goal,
      std::optional<rmf_traffic::Duration> tail_period,
      rmf_task::events::SimpleEventStatePtr state,
      std::function<void()> update,
      std::function<void()> finished);

    ConstStatePtr state() const final;

    rmf_traffic::Duration remaining_time_estimate() const final;

    Backup backup() const final;

    Resume interrupt(std::function<void()> task_is_interrupted) final;

    void cancel() final;

    void kill() final;

  private:

    void _find_plan();

    void _execute_plan(rmf_traffic::agv::Plan new_plan);

    Negotiator::NegotiatePtr _respond(
      const Negotiator::TableViewerPtr& table_view,
      const Negotiator::ResponderPtr& responder);

    AssignIDPtr _assign_id;
    agv::RobotContextPtr _context;
    rmf_traffic::agv::Plan::Goal _goal;
    std::optional<rmf_traffic::Duration> _tail_period;
    std::function<void()> _update;
    std::function<void()> _finished;
    rmf_task::events::SimpleEventStatePtr _state;
    std::shared_ptr<Negotiator> _negotiator;
    std::optional<rmf_traffic::agv::Plan> _plan;
    rmf_task_sequence::Event::ActivePtr _sequence;
    std::shared_ptr<services::FindPath> _find_path_service;
    rclcpp::TimerBase::SharedPtr _find_path_timer;

    bool _is_interrupted = false;
  };
};

} // namespace events
} // namespace rmf_fleet_adapter

#endif // SRC__RMF_FLEET_ADAPTER__EVENTS__GOTOPLACE_HPP