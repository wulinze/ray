// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/raylet/task_dependency_manager.h"

#include "absl/time/clock.h"
#include "ray/stats/stats.h"

namespace ray {

namespace raylet {

TaskDependencyManager::TaskDependencyManager(
    ObjectManagerInterface &object_manager,
    ReconstructionPolicyInterface &reconstruction_policy)
    : object_manager_(object_manager), reconstruction_policy_(reconstruction_policy) {}

bool TaskDependencyManager::CheckObjectLocal(const ObjectID &object_id) const {
  return local_objects_.count(object_id) == 1;
}

bool TaskDependencyManager::CheckObjectRequired(const ObjectID &object_id,
                                                rpc::Address *owner_address) const {
  const TaskID task_id = object_id.TaskId();
  auto task_entry = required_tasks_.find(task_id);
  // If there are no subscribed tasks that are dependent on the object, then do
  // nothing.
  if (task_entry == required_tasks_.end()) {
    return false;
  }
  if (task_entry->second.count(object_id) == 0) {
    return false;
  }
  // If the object is already local, then the dependency is fulfilled. Do
  // nothing.
  if (local_objects_.count(object_id) == 1) {
    return false;
  }
  // If the task that creates the object is pending execution, then the
  // dependency will be fulfilled locally. Do nothing.
  if (pending_tasks_.count(task_id) == 1) {
    return false;
  }
  if (owner_address != nullptr) {
    *owner_address = task_entry->second.at(object_id).owner_address;
  }
  return true;
}

void TaskDependencyManager::HandleRemoteDependencyRequired(const ObjectID &object_id) {
  rpc::Address owner_address;
  bool required = CheckObjectRequired(object_id, &owner_address);
  // If the object is required, then try to make the object available locally.
  if (required) {
    auto inserted = required_objects_.insert(object_id);
    if (inserted.second) {
      // If we haven't already, request the object manager to pull it from a
      // remote node.
      RAY_CHECK_OK(object_manager_.Pull(object_id, owner_address));
      reconstruction_policy_.ListenAndMaybeReconstruct(object_id, owner_address);
    }
  }
}

void TaskDependencyManager::HandleRemoteDependencyCanceled(const ObjectID &object_id) {
  bool required = CheckObjectRequired(object_id, nullptr);
  // If the object is no longer required, then cancel the object.
  if (!required) {
    auto it = required_objects_.find(object_id);
    if (it != required_objects_.end()) {
      object_manager_.CancelPull(object_id);
      reconstruction_policy_.Cancel(object_id);
      required_objects_.erase(it);
    }
  }
}

std::vector<TaskID> TaskDependencyManager::HandleObjectLocal(
    const ray::ObjectID &object_id) {
  // Add the object to the table of locally available objects.
  auto inserted = local_objects_.insert(object_id);
  RAY_CHECK(inserted.second) << object_id;

  // Find all tasks and workers that depend on the newly available object.
  std::vector<TaskID> ready_task_ids;
  auto creating_task_entry = required_tasks_.find(object_id.TaskId());
  if (creating_task_entry != required_tasks_.end()) {
    auto object_entry = creating_task_entry->second.find(object_id);
    if (object_entry != creating_task_entry->second.end()) {
      // Loop through all tasks that depend on the newly available object.
      for (const auto &dependent_task_id : object_entry->second.dependent_tasks) {
        auto &task_entry = task_dependencies_[dependent_task_id];
        task_entry.num_missing_get_dependencies--;
        // If the dependent task now has all of its arguments ready, it's ready
        // to run.
        if (task_entry.num_missing_get_dependencies == 0) {
          ready_task_ids.push_back(dependent_task_id);
        }
      }
      // Remove the dependency from all workers that called `ray.wait` on the
      // newly available object.
      for (const auto &worker_id : object_entry->second.dependent_workers) {
        RAY_CHECK(worker_dependencies_[worker_id].erase(object_id) > 0);
      }
      // Clear all workers that called `ray.wait` on this object, since the
      // `ray.wait` calls can now return the object as ready.
      object_entry->second.dependent_workers.clear();

      // If there are no more tasks or workers dependent on the local object or
      // the task that created it, then remove the entry completely.
      if (object_entry->second.Empty()) {
        creating_task_entry->second.erase(object_entry);
        if (creating_task_entry->second.empty()) {
          required_tasks_.erase(creating_task_entry);
        }
      }
    }
  }

  // The object is now local, so cancel any in-progress operations to make the
  // object local.
  HandleRemoteDependencyCanceled(object_id);

  return ready_task_ids;
}

std::vector<TaskID> TaskDependencyManager::HandleObjectMissing(
    const ray::ObjectID &object_id) {
  // Remove the object from the table of locally available objects.
  auto erased = local_objects_.erase(object_id);
  RAY_CHECK(erased == 1);

  // Find any tasks that are dependent on the missing object.
  std::vector<TaskID> waiting_task_ids;
  TaskID creating_task_id = object_id.TaskId();
  auto creating_task_entry = required_tasks_.find(creating_task_id);
  if (creating_task_entry != required_tasks_.end()) {
    auto object_entry = creating_task_entry->second.find(object_id);
    if (object_entry != creating_task_entry->second.end()) {
      for (auto &dependent_task_id : object_entry->second.dependent_tasks) {
        auto &task_entry = task_dependencies_[dependent_task_id];
        // If the dependent task had all of its arguments ready, it was ready to
        // run but must be switched to waiting since one of its arguments is now
        // missing.
        if (task_entry.num_missing_get_dependencies == 0) {
          waiting_task_ids.push_back(dependent_task_id);
          // During normal execution we should be able to include the check
          // RAY_CHECK(pending_tasks_.count(dependent_task_id) == 1);
          // However, this invariant will not hold during unit test execution.
        }
        task_entry.num_missing_get_dependencies++;
      }
    }
  }
  // The object is no longer local. Try to make the object local if necessary.
  HandleRemoteDependencyRequired(object_id);
  // Process callbacks for all of the tasks dependent on the object that are
  // now ready to run.
  return waiting_task_ids;
}

bool TaskDependencyManager::SubscribeGetDependencies(
    const TaskID &task_id, const std::vector<rpc::ObjectReference> &required_objects) {
  auto &task_entry = task_dependencies_[task_id];

  // Record the task's dependencies.
  for (const auto &object : required_objects) {
    const auto &object_id = ObjectID::FromBinary(object.object_id());
    auto inserted = task_entry.get_dependencies.insert(object_id);
    if (inserted.second) {
      RAY_LOG(DEBUG) << "Task " << task_id << " blocked on object " << object_id;
      // Get the ID of the task that creates the dependency.
      TaskID creating_task_id = object_id.TaskId();
      // Determine whether the dependency can be fulfilled by the local node.
      if (local_objects_.count(object_id) == 0) {
        // The object is not local.
        task_entry.num_missing_get_dependencies++;
      }

      auto it = required_tasks_[creating_task_id].find(object_id);
      if (it == required_tasks_[creating_task_id].end()) {
        it = required_tasks_[creating_task_id]
                 .emplace(object_id, ObjectDependencies(object))
                 .first;
      }
      // Add the subscribed task to the mapping from object ID to list of
      // dependent tasks.
      it->second.dependent_tasks.insert(task_id);
    }
  }

  // These dependencies are required by the given task. Try to make them local
  // if necessary.
  for (const auto &object : required_objects) {
    const auto &object_id = ObjectID::FromBinary(object.object_id());
    HandleRemoteDependencyRequired(object_id);
  }

  // Return whether all dependencies are local.
  return (task_entry.num_missing_get_dependencies == 0);
}

void TaskDependencyManager::SubscribeWaitDependencies(
    const WorkerID &worker_id,
    const std::vector<rpc::ObjectReference> &required_objects) {
  auto &worker_entry = worker_dependencies_[worker_id];

  // Record the worker's dependencies.
  for (const auto &object : required_objects) {
    const auto &object_id = ObjectID::FromBinary(object.object_id());
    if (local_objects_.count(object_id) == 0) {
      RAY_LOG(DEBUG) << "Worker " << worker_id << " called ray.wait on remote object "
                     << object_id;
      // Only add the dependency if the object is not local. If the object is
      // local, then the `ray.wait` call can already return it.
      auto inserted = worker_entry.insert(object_id);
      if (inserted.second) {
        // Get the ID of the task that creates the dependency.
        TaskID creating_task_id = object_id.TaskId();
        auto it = required_tasks_[creating_task_id].find(object_id);
        if (it == required_tasks_[creating_task_id].end()) {
          it = required_tasks_[creating_task_id]
                   .emplace(object_id, ObjectDependencies(object))
                   .first;
        }
        // Add the subscribed worker to the mapping from object ID to list of
        // dependent workers.
        it->second.dependent_workers.insert(worker_id);
      }
    }
  }

  // These dependencies are required by the given worker. Try to make them
  // local if necessary.
  for (const auto &object : required_objects) {
    const auto &object_id = ObjectID::FromBinary(object.object_id());
    HandleRemoteDependencyRequired(object_id);
  }
}

bool TaskDependencyManager::UnsubscribeGetDependencies(const TaskID &task_id) {
  RAY_LOG(DEBUG) << "Task " << task_id << " no longer blocked";
  // Remove the task from the table of subscribed tasks.
  auto it = task_dependencies_.find(task_id);
  if (it == task_dependencies_.end()) {
    return false;
  }
  const TaskDependencies task_entry = std::move(it->second);
  task_dependencies_.erase(it);

  // Remove the task's dependencies.
  for (const auto &object_id : task_entry.get_dependencies) {
    // Get the ID of the task that creates the dependency.
    TaskID creating_task_id = object_id.TaskId();
    auto creating_task_entry = required_tasks_.find(creating_task_id);
    // Remove the task from the list of tasks that are dependent on this
    // object.
    auto it = creating_task_entry->second.find(object_id);
    RAY_CHECK(it != creating_task_entry->second.end());
    RAY_CHECK(it->second.dependent_tasks.erase(task_id) > 0);
    // If nothing else depends on the object, then erase the object entry.
    if (it->second.Empty()) {
      creating_task_entry->second.erase(it);
      // Remove the task that creates this object if there are no more object
      // dependencies created by the task.
      if (creating_task_entry->second.empty()) {
        required_tasks_.erase(creating_task_entry);
      }
    }
  }

  // These dependencies are no longer required by the given task. Cancel any
  // in-progress operations to make them local.
  for (const auto &object_id : task_entry.get_dependencies) {
    HandleRemoteDependencyCanceled(object_id);
  }

  return true;
}

void TaskDependencyManager::UnsubscribeWaitDependencies(const WorkerID &worker_id) {
  RAY_LOG(DEBUG) << "Worker " << worker_id << " no longer blocked";
  // Remove the task from the table of subscribed tasks.
  auto it = worker_dependencies_.find(worker_id);
  if (it == worker_dependencies_.end()) {
    return;
  }
  const WorkerDependencies worker_entry = std::move(it->second);
  worker_dependencies_.erase(it);

  // Remove the task's dependencies.
  for (const auto &object_id : worker_entry) {
    // Get the ID of the task that creates the dependency.
    TaskID creating_task_id = object_id.TaskId();
    auto creating_task_entry = required_tasks_.find(creating_task_id);
    // Remove the worker from the list of workers that are dependent on this
    // object.
    auto it = creating_task_entry->second.find(object_id);
    RAY_CHECK(it != creating_task_entry->second.end());
    RAY_CHECK(it->second.dependent_workers.erase(worker_id) > 0);
    // If nothing else depends on the object, then erase the object entry.
    if (it->second.Empty()) {
      creating_task_entry->second.erase(it);
      // Remove the task that creates this object if there are no more object
      // dependencies created by the task.
      if (creating_task_entry->second.empty()) {
        required_tasks_.erase(creating_task_entry);
      }
    }
  }

  // These dependencies are no longer required by the given task. Cancel any
  // in-progress operations to make them local.
  for (const auto &object_id : worker_entry) {
    HandleRemoteDependencyCanceled(object_id);
  }
}

void TaskDependencyManager::TaskPending(const Task &task) {
  // Direct tasks are not tracked by the raylet.
  // NOTE(zhijunfu): Direct tasks are not tracked by the raylet,
  // but we still need raylet to reconstruct the actors.
  // For direct actor creation task:
  //   - Initially the caller leases a worker from raylet and
  //     then pushes actor creation task directly to the worker,
  //     thus it doesn't need task lease. And actually if we
  //     acquire a lease in this case and forget to cancel it,
  //     the lease would never expire which will prevent the
  //     actor from being restarted;
  //   - When a direct actor is restarted, raylet resubmits
  //     the task, and the task can be forwarded to another raylet,
  //     and eventually assigned to a worker. In this case we need
  //     the task lease to make sure there's only one raylet can
  //     resubmit the task.
  //
  // We can use `OnDispatch` to differeniate whether this task is
  // a worker lease request.
  // For direct actor creation task:
  //   - when it's submitted by core worker, we guarantee that
  //     we always request a new worker lease, in that case
  //     `OnDispatch` is overridden to an actual callback.
  //   - when it's resubmitted by raylet because of reconstruction,
  //     `OnDispatch` will not be overridden and thus is nullptr.
  if (task.GetTaskSpecification().IsActorCreationTask() && task.OnDispatch() == nullptr) {
    // This is an actor creation task, and it's being restarted,
    // in this case we still need the task lease. Note that we don't
    // require task lease for direct actor creation task.
  } else {
    return;
  }

  TaskID task_id = task.GetTaskSpecification().TaskId();
  RAY_LOG(DEBUG) << "Task execution " << task_id << " pending";

  // Record that the task is pending execution.
  auto inserted = pending_tasks_.insert(task_id);
  if (inserted.second) {
    // This is the first time we've heard that this task is pending.  Find any
    // subscribed tasks that are dependent on objects created by the pending
    // task.
    auto remote_task_entry = required_tasks_.find(task_id);
    if (remote_task_entry != required_tasks_.end()) {
      for (const auto &object_entry : remote_task_entry->second) {
        // This object created by the pending task will appear locally once the
        // task completes execution. Cancel any in-progress operations to make
        // the object local.
        HandleRemoteDependencyCanceled(object_entry.first);
      }
    }
  }
}

void TaskDependencyManager::TaskCanceled(const TaskID &task_id) {
  RAY_LOG(DEBUG) << "Task execution " << task_id << " canceled";
  // Record that the task is no longer pending execution.
  auto it = pending_tasks_.find(task_id);
  if (it == pending_tasks_.end()) {
    return;
  }
  pending_tasks_.erase(it);

  // Find any subscribed tasks that are dependent on objects created by the
  // canceled task.
  auto remote_task_entry = required_tasks_.find(task_id);
  if (remote_task_entry != required_tasks_.end()) {
    for (const auto &object_entry : remote_task_entry->second) {
      // This object created by the task will no longer appear locally since
      // the task is canceled.  Try to make the object local if necessary.
      HandleRemoteDependencyRequired(object_entry.first);
    }
  }
}

void TaskDependencyManager::RemoveTasksAndRelatedObjects(
    const std::unordered_set<TaskID> &task_ids) {
  // Collect a list of all the unique objects that these tasks were subscribed
  // to.
  std::unordered_set<ObjectID> required_objects;
  for (auto it = task_ids.begin(); it != task_ids.end(); it++) {
    auto task_it = task_dependencies_.find(*it);
    if (task_it != task_dependencies_.end()) {
      // Add the objects that this task was subscribed to.
      required_objects.insert(task_it->second.get_dependencies.begin(),
                              task_it->second.get_dependencies.end());
    }
    // The task no longer depends on anything.
    task_dependencies_.erase(*it);
    // The task is no longer pending execution.
    pending_tasks_.erase(*it);
  }

  // Cancel all of the objects that were required by the removed tasks.
  for (const auto &object_id : required_objects) {
    TaskID creating_task_id = object_id.TaskId();
    required_tasks_.erase(creating_task_id);
    HandleRemoteDependencyCanceled(object_id);
  }

  // Make sure that the tasks in task_ids no longer have tasks dependent on
  // them.
  for (const auto &task_id : task_ids) {
    RAY_CHECK(required_tasks_.find(task_id) == required_tasks_.end())
        << "RemoveTasksAndRelatedObjects was called on " << task_id
        << ", but another task depends on it that was not included in the argument";
  }
}

std::string TaskDependencyManager::DebugString() const {
  std::stringstream result;
  result << "TaskDependencyManager:";
  result << "\n- task dep map size: " << task_dependencies_.size();
  result << "\n- task req map size: " << required_tasks_.size();
  result << "\n- req objects map size: " << required_objects_.size();
  result << "\n- local objects map size: " << local_objects_.size();
  result << "\n- pending tasks map size: " << pending_tasks_.size();
  return result.str();
}

void TaskDependencyManager::RecordMetrics() const {
  stats::NumSubscribedTasks().Record(task_dependencies_.size());
  stats::NumRequiredTasks().Record(required_tasks_.size());
  stats::NumRequiredObjects().Record(required_objects_.size());
  stats::NumPendingTasks().Record(pending_tasks_.size());
}

bool TaskDependencyManager::GetOwnerAddress(const ObjectID &object_id,
                                            rpc::Address *owner_address) const {
  const auto creating_task_entry = required_tasks_.find(object_id.TaskId());
  if (creating_task_entry == required_tasks_.end()) {
    return false;
  }

  const auto it = creating_task_entry->second.find(object_id);
  if (it == creating_task_entry->second.end()) {
    return false;
  }

  *owner_address = it->second.owner_address;
  return !owner_address->worker_id().empty();
}

}  // namespace raylet

}  // namespace ray
