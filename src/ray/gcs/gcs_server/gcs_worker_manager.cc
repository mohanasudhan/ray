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

#include "ray/gcs/gcs_server/gcs_worker_manager.h"

#include "ray/stats/metric_defs.h"

namespace ray {
namespace gcs {

void GcsWorkerManager::HandleReportWorkerFailure(
    rpc::ReportWorkerFailureRequest request,
    rpc::ReportWorkerFailureReply *reply,
    rpc::SendReplyCallback send_reply_callback) {
  const rpc::Address worker_address = request.worker_failure().worker_address();
  const auto worker_id = WorkerID::FromBinary(worker_address.worker_id());
  GetWorkerInfo(
      worker_id,
      [this,
       reply,
       send_reply_callback,
       worker_id = std::move(worker_id),
       request = std::move(request),
       worker_address =
           std::move(worker_address)](const boost::optional<WorkerTableData> &result) {
        const auto node_id = NodeID::FromBinary(worker_address.raylet_id());
        std::string message =
            absl::StrCat("Reporting worker exit, worker id = ",
                         worker_id.Hex(),
                         ", node id = ",
                         node_id.Hex(),
                         ", address = ",
                         worker_address.ip_address(),
                         ", exit_type = ",
                         rpc::WorkerExitType_Name(request.worker_failure().exit_type()),
                         ", exit_detail = ",
                         request.worker_failure().exit_detail());
        if (request.worker_failure().exit_type() ==
                rpc::WorkerExitType::INTENDED_USER_EXIT ||
            request.worker_failure().exit_type() ==
                rpc::WorkerExitType::INTENDED_SYSTEM_EXIT) {
          RAY_LOG(DEBUG) << message;
        } else {
          RAY_LOG(WARNING)
              << message
              << ". Unintentional worker failures have been reported. If there "
                 "are lots of this logs, that might indicate there are "
                 "unexpected failures in the cluster.";
        }
        auto worker_failure_data = std::make_shared<WorkerTableData>();
        if (result) {
          worker_failure_data->CopyFrom(*result);
        }
        worker_failure_data->MergeFrom(request.worker_failure());
        worker_failure_data->set_is_alive(false);

        for (auto &listener : worker_dead_listeners_) {
          listener(worker_failure_data);
        }

        auto on_done = [this,
                        worker_address,
                        worker_id,
                        node_id,
                        worker_failure_data,
                        reply,
                        send_reply_callback](const Status &status) {
          if (!status.ok()) {
            RAY_LOG(ERROR) << "Failed to report worker failure, worker id = " << worker_id
                           << ", node id = " << node_id
                           << ", address = " << worker_address.ip_address();
          } else {
            stats::UnintentionalWorkerFailures.Record(1);
            // Only publish worker_id and raylet_id in address as they are the only fields
            // used by sub clients.
            rpc::WorkerDeltaData worker_failure;
            worker_failure.set_worker_id(
                worker_failure_data->worker_address().worker_id());
            worker_failure.set_raylet_id(
                worker_failure_data->worker_address().raylet_id());
            RAY_CHECK_OK(
                gcs_publisher_->PublishWorkerFailure(worker_id, worker_failure, nullptr));
          }
          GCS_RPC_SEND_REPLY(send_reply_callback, reply, status);
        };

        // As soon as the worker starts, it will register with GCS. It ensures that GCS
        // receives the worker registration information first and then the worker failure
        // message, so we delete the get operation. Related issues:
        // https://github.com/ray-project/ray/pull/11599
        Status status = gcs_table_storage_->WorkerTable().Put(
            worker_id, *worker_failure_data, on_done);
        if (!status.ok()) {
          on_done(status);
        }

        if (request.worker_failure().exit_type() == rpc::WorkerExitType::SYSTEM_ERROR ||
            request.worker_failure().exit_type() ==
                rpc::WorkerExitType::NODE_OUT_OF_MEMORY) {
          usage::TagKey key;
          int count = 0;
          if (request.worker_failure().exit_type() == rpc::WorkerExitType::SYSTEM_ERROR) {
            worker_crash_system_error_count_ += 1;
            key = usage::TagKey::WORKER_CRASH_SYSTEM_ERROR;
            count = worker_crash_system_error_count_;
          } else if (request.worker_failure().exit_type() ==
                     rpc::WorkerExitType::NODE_OUT_OF_MEMORY) {
            worker_crash_oom_count_ += 1;
            key = usage::TagKey::WORKER_CRASH_OOM;
            count = worker_crash_oom_count_;
          }
          if (usage_stats_client_) {
            usage_stats_client_->RecordExtraUsageCounter(key, count);
          }
        }
      });
}

void GcsWorkerManager::HandleGetWorkerInfo(rpc::GetWorkerInfoRequest request,
                                           rpc::GetWorkerInfoReply *reply,
                                           rpc::SendReplyCallback send_reply_callback) {
  WorkerID worker_id = WorkerID::FromBinary(request.worker_id());
  RAY_LOG(DEBUG) << "Getting worker info, worker id = " << worker_id;

  GetWorkerInfo(worker_id,
                [reply, send_reply_callback, worker_id = std::move(worker_id)](
                    const boost::optional<WorkerTableData> &result) {
                  if (result) {
                    reply->mutable_worker_table_data()->CopyFrom(*result);
                  }
                  RAY_LOG(DEBUG)
                      << "Finished getting worker info, worker id = " << worker_id;
                  GCS_RPC_SEND_REPLY(send_reply_callback, reply, Status::OK());
                });
}

void GcsWorkerManager::HandleGetAllWorkerInfo(
    rpc::GetAllWorkerInfoRequest request,
    rpc::GetAllWorkerInfoReply *reply,
    rpc::SendReplyCallback send_reply_callback) {
  auto limit = request.has_limit() ? request.limit() : -1;

  RAY_LOG(DEBUG) << "Getting all worker info.";
  auto on_done = [reply, send_reply_callback, limit](
                     const absl::flat_hash_map<WorkerID, WorkerTableData> &result) {
    auto total_workers = result.size();
    reply->set_total(total_workers);

    auto count = 0;
    for (auto &data : result) {
      if (limit != -1 && count >= limit) {
        break;
      }
      count += 1;

      reply->add_worker_table_data()->CopyFrom(data.second);
    }
    RAY_LOG(DEBUG) << "Finished getting all worker info.";
    GCS_RPC_SEND_REPLY(send_reply_callback, reply, Status::OK());
  };
  Status status = gcs_table_storage_->WorkerTable().GetAll(on_done);
  if (!status.ok()) {
    on_done(absl::flat_hash_map<WorkerID, WorkerTableData>());
  }
}

void GcsWorkerManager::HandleAddWorkerInfo(rpc::AddWorkerInfoRequest request,
                                           rpc::AddWorkerInfoReply *reply,
                                           rpc::SendReplyCallback send_reply_callback) {
  auto worker_data = std::make_shared<WorkerTableData>();
  worker_data->CopyFrom(request.worker_data());
  auto worker_id = WorkerID::FromBinary(worker_data->worker_address().worker_id());
  RAY_LOG(DEBUG) << "Adding worker " << worker_id;

  auto on_done =
      [worker_id, worker_data, reply, send_reply_callback](const Status &status) {
        if (!status.ok()) {
          RAY_LOG(ERROR) << "Failed to add worker information, "
                         << worker_data->DebugString();
        }
        RAY_LOG(DEBUG) << "Finished adding worker " << worker_id;
        GCS_RPC_SEND_REPLY(send_reply_callback, reply, status);
      };

  Status status = gcs_table_storage_->WorkerTable().Put(worker_id, *worker_data, on_done);
  if (!status.ok()) {
    on_done(status);
  }
}

void GcsWorkerManager::AddWorkerDeadListener(
    std::function<void(std::shared_ptr<WorkerTableData>)> listener) {
  RAY_CHECK(listener != nullptr);
  worker_dead_listeners_.emplace_back(std::move(listener));
}

void GcsWorkerManager::GetWorkerInfo(
    const WorkerID &worker_id,
    std::function<void(const boost::optional<WorkerTableData> &)> callback) const {
  auto on_done = [worker_id, callback = std::move(callback)](
                     const Status &status,
                     const boost::optional<WorkerTableData> &result) {
    if (!status.ok()) {
      RAY_LOG(WARNING) << "Failed to get worker info, worker id = " << worker_id
                       << ", status = " << status;
      callback(boost::none);
    } else {
      callback(result);
    }
  };

  Status status = gcs_table_storage_->WorkerTable().Get(worker_id, on_done);
  if (!status.ok()) {
    on_done(status, boost::none);
  }
}

}  // namespace gcs
}  // namespace ray
