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

#include "ray/raylet/worker_pool.h"

#include <algorithm>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <deque>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/strings/str_split.h"
#include "ray/common/constants.h"
#include "ray/common/network_util.h"
#include "ray/common/ray_config.h"
#include "ray/common/runtime_env_common.h"
#include "ray/common/status.h"
#include "ray/common/task/task_spec.h"
#include "ray/core_worker/common.h"
#include "ray/gcs/pb_util.h"
#include "ray/stats/metric_defs.h"
#include "ray/util/logging.h"
#include "ray/util/util.h"

DEFINE_stats(worker_register_time_ms,
             "end to end latency of register a worker process.",
             (),
             ({1, 10, 100, 1000, 10000}),
             ray::stats::HISTOGRAM);

namespace {

std::shared_ptr<ray::raylet::WorkerInterface> GetWorker(
    const std::unordered_set<std::shared_ptr<ray::raylet::WorkerInterface>> &worker_pool,
    const std::shared_ptr<ray::ClientConnection> &connection) {
  for (auto it = worker_pool.begin(); it != worker_pool.end(); it++) {
    if ((*it)->Connection() == connection) {
      return (*it);
    }
  }
  return nullptr;
}

std::shared_ptr<ray::raylet::WorkerInterface> GetWorker(
    const std::unordered_set<std::shared_ptr<ray::raylet::WorkerInterface>> &worker_pool,
    const WorkerID &worker_id) {
  for (auto it = worker_pool.begin(); it != worker_pool.end(); it++) {
    if ((*it)->WorkerId() == worker_id) {
      return (*it);
    }
  }
  return nullptr;
}

// Remove the worker from the set, returning true if it was present.
bool RemoveWorker(
    std::unordered_set<std::shared_ptr<ray::raylet::WorkerInterface>> &worker_pool,
    const std::shared_ptr<ray::raylet::WorkerInterface> &worker) {
  return worker_pool.erase(worker) > 0;
}

// Return true if the optionals' values match or if either of them is empty.
bool OptionalsMatchOrEitherEmpty(const std::optional<bool> &ask,
                                 const std::optional<bool> &have) {
  return !ask.has_value() || !have.has_value() || ask.value() == have.value();
}

}  // namespace

namespace ray {

namespace raylet {

WorkerPool::WorkerPool(instrumented_io_context &io_service,
                       const NodeID &node_id,
                       std::string node_address,
                       std::function<int64_t()> get_num_cpus_available,
                       int num_prestarted_python_workers,
                       int maximum_startup_concurrency,
                       int min_worker_port,
                       int max_worker_port,
                       const std::vector<int> &worker_ports,
                       gcs::GcsClient &gcs_client,
                       const WorkerCommandMap &worker_commands,
                       std::string native_library_path,
                       std::function<void()> starting_worker_timeout_callback,
                       int ray_debugger_external,
                       std::function<absl::Time()> get_time,
                       bool enable_resource_isolation)
    : worker_startup_token_counter_(0),
      io_service_(&io_service),
      node_id_(node_id),
      node_address_(std::move(node_address)),
      get_num_cpus_available_(std::move(get_num_cpus_available)),
      maximum_startup_concurrency_(
          RayConfig::instance().worker_maximum_startup_concurrency() > 0
              ?
              // Overwrite the maximum concurrency.
              RayConfig::instance().worker_maximum_startup_concurrency()
              : maximum_startup_concurrency),
      gcs_client_(gcs_client),
      native_library_path_(std::move(native_library_path)),
      starting_worker_timeout_callback_(std::move(starting_worker_timeout_callback)),
      ray_debugger_external(ray_debugger_external),
      first_job_registered_python_worker_count_(0),
      first_job_driver_wait_num_python_workers_(
          std::min(num_prestarted_python_workers, maximum_startup_concurrency_)),
      num_prestart_python_workers(num_prestarted_python_workers),
      periodical_runner_(PeriodicalRunner::Create(io_service)),
      get_time_(std::move(get_time)),
      enable_resource_isolation_(enable_resource_isolation) {
  RAY_CHECK_GT(maximum_startup_concurrency_, 0);
  // We need to record so that the metric exists. This way, we report that 0
  // processes have started before a task runs on the node (as opposed to the
  // metric not existing at all).
  stats::NumWorkersStarted.Record(0);
  stats::NumWorkersStartedFromCache.Record(0);
  stats::NumCachedWorkersSkippedJobMismatch.Record(0);
  stats::NumCachedWorkersSkippedDynamicOptionsMismatch.Record(0);
  stats::NumCachedWorkersSkippedRuntimeEnvironmentMismatch.Record(0);
  // We used to ignore SIGCHLD here. The code is moved to raylet main.cc to support the
  // subreaper feature.
  for (const auto &entry : worker_commands) {
    // Initialize the pool state for this language.
    auto &state = states_by_lang_[entry.first];
    state.multiple_for_warning = maximum_startup_concurrency_;
    // Set worker command for this language.
    state.worker_command = entry.second;
    RAY_CHECK(!state.worker_command.empty()) << "Worker command must not be empty.";
  }
  // Initialize free ports list with all ports in the specified range.
  if (!worker_ports.empty()) {
    free_ports_ = std::make_unique<std::queue<int>>();
    for (int port : worker_ports) {
      free_ports_->push(port);
    }
  } else if (min_worker_port != 0) {
    if (max_worker_port == 0) {
      max_worker_port = 65535;  // Maximum valid port number.
    }
    RAY_CHECK(min_worker_port > 0 && min_worker_port <= 65535);
    RAY_CHECK(max_worker_port >= min_worker_port && max_worker_port <= 65535);
    free_ports_ = std::make_unique<std::queue<int>>();
    for (int port = min_worker_port; port <= max_worker_port; port++) {
      free_ports_->push(port);
    }
  }
}

WorkerPool::~WorkerPool() {
  std::unordered_set<Process> procs_to_kill;
  for (const auto &entry : states_by_lang_) {
    // Kill all the worker processes.
    for (auto &worker_process : entry.second.worker_processes) {
      procs_to_kill.insert(worker_process.second.proc);
    }
  }
  for (Process proc : procs_to_kill) {
    proc.Kill();
    // NOTE: Avoid calling Wait() here. It fails with ECHILD, as SIGCHLD is disabled.
  }
}

void WorkerPool::Start() {
  if (RayConfig::instance().kill_idle_workers_interval_ms() > 0) {
    periodical_runner_->RunFnPeriodically(
        [this] { TryKillingIdleWorkers(); },
        RayConfig::instance().kill_idle_workers_interval_ms(),
        "RayletWorkerPool.deadline_timer.kill_idle_workers");
  }

  if (RayConfig::instance().enable_worker_prestart()) {
    rpc::TaskSpec rpc_task_spec;
    rpc_task_spec.set_language(Language::PYTHON);
    rpc_task_spec.mutable_runtime_env_info()->set_serialized_runtime_env("{}");

    TaskSpecification task_spec{std::move(rpc_task_spec)};
    PrestartWorkersInternal(task_spec, num_prestart_python_workers);
  }
}

// NOTE(kfstorm): The node manager cannot be passed via WorkerPool constructor because the
// grpc server is started after the WorkerPool instance is constructed.
void WorkerPool::SetNodeManagerPort(int node_manager_port) {
  node_manager_port_ = node_manager_port;
}

void WorkerPool::SetRuntimeEnvAgentClient(
    std::unique_ptr<RuntimeEnvAgentClient> runtime_env_agent_client) {
  if (!runtime_env_agent_client) {
    RAY_LOG(FATAL) << "SetRuntimeEnvAgentClient requires non empty pointer";
  }
  runtime_env_agent_client_ = std::move(runtime_env_agent_client);
}

void WorkerPool::PopWorkerCallbackAsync(PopWorkerCallback callback,
                                        std::shared_ptr<WorkerInterface> worker,
                                        PopWorkerStatus status) {
  // This method shouldn't be invoked when runtime env creation has failed because
  // when runtime env is failed to be created, they are all
  // invoking the callback immediately.
  RAY_CHECK(status != PopWorkerStatus::RuntimeEnvCreationFailed);
  // Call back this function asynchronously to make sure executed in different stack.
  io_service_->post(
      [this, callback = std::move(callback), worker = std::move(worker), status]() {
        PopWorkerCallbackInternal(callback, worker, status);
      },
      "WorkerPool.PopWorkerCallback");
}

void WorkerPool::PopWorkerCallbackInternal(const PopWorkerCallback &callback,
                                           std::shared_ptr<WorkerInterface> worker,
                                           PopWorkerStatus status) {
  RAY_CHECK(callback);
  auto used = callback(worker, status, /*runtime_env_setup_error_message=*/"");
  if (worker && !used) {
    // The invalid worker not used, restore it to worker pool.
    PushWorker(worker);
  }
}

void WorkerPool::update_worker_startup_token_counter() {
  worker_startup_token_counter_ += 1;
}

void WorkerPool::AddWorkerProcess(
    State &state,
    rpc::WorkerType worker_type,
    const Process &proc,
    const std::chrono::high_resolution_clock::time_point &start,
    const rpc::RuntimeEnvInfo &runtime_env_info,
    const std::vector<std::string> &dynamic_options,
    std::optional<absl::Duration> worker_startup_keep_alive_duration) {
  state.worker_processes.emplace(worker_startup_token_counter_,
                                 WorkerProcessInfo{/*is_pending_registration=*/true,
                                                   worker_type,
                                                   proc,
                                                   start,
                                                   runtime_env_info,
                                                   dynamic_options,
                                                   worker_startup_keep_alive_duration});
}

void WorkerPool::RemoveWorkerProcess(State &state,
                                     const StartupToken &proc_startup_token) {
  state.worker_processes.erase(proc_startup_token);
}

std::pair<std::vector<std::string>, ProcessEnvironment>
WorkerPool::BuildProcessCommandArgs(const Language &language,
                                    rpc::JobConfig *job_config,
                                    const rpc::WorkerType worker_type,
                                    const JobID &job_id,
                                    const std::vector<std::string> &dynamic_options,
                                    const int runtime_env_hash,
                                    const std::string &serialized_runtime_env_context,
                                    const WorkerPool::State &state) const {
  std::vector<std::string> options;

  // Append Ray-defined per-job options here
  std::string code_search_path;
  if (language == Language::JAVA || language == Language::CPP) {
    if (job_config) {
      std::string code_search_path_str;
      for (int i = 0; i < job_config->code_search_path_size(); i++) {
        auto path = job_config->code_search_path(i);
        if (i != 0) {
          code_search_path_str += ":";
        }
        code_search_path_str += path;
      }
      if (!code_search_path_str.empty()) {
        code_search_path = code_search_path_str;
        if (language == Language::JAVA) {
          code_search_path_str = "-Dray.job.code-search-path=" + code_search_path_str;
        } else if (language == Language::CPP) {
          code_search_path_str = "--ray_code_search_path=" + code_search_path_str;
        } else {
          RAY_LOG(FATAL) << "Unknown language " << Language_Name(language);
        }
        options.push_back(code_search_path_str);
      }
    }
  }

  // Append user-defined per-job options here
  if (language == Language::JAVA) {
    if (!job_config->jvm_options().empty()) {
      options.insert(options.end(),
                     job_config->jvm_options().begin(),
                     job_config->jvm_options().end());
    }
  }

  // Append startup-token for JAVA here
  if (language == Language::JAVA) {
    options.push_back("-Dray.raylet.startup-token=" +
                      std::to_string(worker_startup_token_counter_));
    options.push_back("-Dray.internal.runtime-env-hash=" +
                      std::to_string(runtime_env_hash));
  }

  // Append user-defined per-process options here
  options.insert(options.end(), dynamic_options.begin(), dynamic_options.end());

  // Extract pointers from the worker command to pass into execvpe.
  std::vector<std::string> worker_command_args;
  for (const auto &token : state.worker_command) {
    if (token == kWorkerDynamicOptionPlaceholder) {
      worker_command_args.insert(
          worker_command_args.end(), options.begin(), options.end());
      continue;
    }
    RAY_CHECK(node_manager_port_ != 0)
        << "Node manager port is not set yet. This shouldn't happen unless we are trying "
           "to start a worker process before node manager server is started. In this "
           "case, it's a bug and it should be fixed.";
    auto node_manager_port_position = token.find(kNodeManagerPortPlaceholder);
    if (node_manager_port_position != std::string::npos) {
      auto replaced_token = token;
      replaced_token.replace(node_manager_port_position,
                             strlen(kNodeManagerPortPlaceholder),
                             std::to_string(node_manager_port_));
      worker_command_args.push_back(std::move(replaced_token));
      continue;
    }
    worker_command_args.push_back(token);
  }

  if (language == Language::PYTHON) {
    RAY_CHECK(worker_type == rpc::WorkerType::WORKER || IsIOWorkerType(worker_type));
    if (IsIOWorkerType(worker_type)) {
      // Without "--worker-type", by default the worker type is rpc::WorkerType::WORKER.
      worker_command_args.push_back("--worker-type=" + rpc::WorkerType_Name(worker_type));
    }
  }

  if (IsIOWorkerType(worker_type)) {
    RAY_CHECK(!RayConfig::instance().object_spilling_config().empty());
    RAY_LOG(DEBUG) << "Adding object spill config "
                   << RayConfig::instance().object_spilling_config();
    worker_command_args.push_back(
        "--object-spilling-config=" +
        absl::Base64Escape(RayConfig::instance().object_spilling_config()));
  }

  if (language == Language::PYTHON) {
    worker_command_args.push_back("--startup-token=" +
                                  std::to_string(worker_startup_token_counter_));
    worker_command_args.push_back("--worker-launch-time-ms=" +
                                  std::to_string(current_sys_time_ms()));
    worker_command_args.push_back("--node-id=" + node_id_.Hex());
    worker_command_args.push_back("--runtime-env-hash=" +
                                  std::to_string(runtime_env_hash));
  } else if (language == Language::CPP) {
    worker_command_args.push_back("--startup_token=" +
                                  std::to_string(worker_startup_token_counter_));
    worker_command_args.push_back("--ray_runtime_env_hash=" +
                                  std::to_string(runtime_env_hash));
  }

  if (serialized_runtime_env_context != "{}" && !serialized_runtime_env_context.empty()) {
    worker_command_args.push_back("--language=" + Language_Name(language));
    worker_command_args.push_back("--serialized-runtime-env-context=" +
                                  serialized_runtime_env_context);
  } else if (language == Language::PYTHON && worker_command_args.size() >= 2 &&
             worker_command_args[1].find(kSetupWorkerFilename) != std::string::npos) {
    // Check that the arg really is the path to the setup worker before erasing it, to
    // prevent breaking tests that mock out the worker command args.
    worker_command_args.erase(worker_command_args.begin() + 1,
                              worker_command_args.begin() + 2);
  } else {
    worker_command_args.push_back("--language=" + Language_Name(language));
  }

  if (ray_debugger_external) {
    worker_command_args.push_back("--ray-debugger-external");
  }

  ProcessEnvironment env;
  if (!IsIOWorkerType(worker_type)) {
    // We pass the job ID to worker processes via an environment variable, so we don't
    // need to add a new CLI parameter for both Python and Java workers.
    env.emplace(kEnvVarKeyJobId, job_id.Hex());
    RAY_LOG(DEBUG) << "Launch worker with " << kEnvVarKeyJobId << " " << job_id.Hex();
  }
  env.emplace(kEnvVarKeyRayletPid, std::to_string(GetPID()));

  // TODO(SongGuyang): Maybe Python and Java also need native library path in future.
  if (language == Language::CPP) {
    // Set native library path for shared library search.
    if (!native_library_path_.empty() || !code_search_path.empty()) {
#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
      auto path_env_p = std::getenv(kLibraryPathEnvName);
      std::string path_env = native_library_path_;
      if (path_env_p != nullptr && strlen(path_env_p) != 0) {
        path_env.append(":").append(path_env_p);
      }
      // Append per-job code search path to library path.
      if (!code_search_path.empty()) {
        path_env.append(":").append(code_search_path);
      }
      auto path_env_iter = env.find(kLibraryPathEnvName);
      if (path_env_iter == env.end()) {
        env.emplace(kLibraryPathEnvName, path_env);
      } else {
        env[kLibraryPathEnvName] = path_env_iter->second.append(":").append(path_env);
      }
#endif
    }
  }

  if (language == Language::PYTHON && worker_type == rpc::WorkerType::WORKER &&
      RayConfig::instance().preload_python_modules().size() > 0) {
    std::string serialized_preload_python_modules =
        absl::StrJoin(RayConfig::instance().preload_python_modules(), ",");
    RAY_LOG(DEBUG) << "Starting worker with preload_python_modules "
                   << serialized_preload_python_modules;
    worker_command_args.push_back("--worker-preload-modules=" +
                                  serialized_preload_python_modules);
  }

  // Pass resource isolation flag to python worker.
  if (language == Language::PYTHON && worker_type == rpc::WorkerType::WORKER) {
    worker_command_args.emplace_back(absl::StrFormat(
        "--enable-resource-isolation=%s", enable_resource_isolation_ ? "true" : "false"));
  }

  // We use setproctitle to change python worker process title,
  // causing the process's /proc/PID/environ being empty.
  // Add `SPT_NOENV` env to prevent setproctitle breaking /proc/PID/environ.
  // Refer this issue for more details: https://github.com/ray-project/ray/issues/15061
  if (language == Language::PYTHON) {
    env.insert({"SPT_NOENV", "1"});
  }

  if (RayConfig::instance().support_fork()) {
    // Support forking in gRPC.
    env.insert({"GRPC_ENABLE_FORK_SUPPORT", "True"});
    env.insert({"GRPC_POLL_STRATEGY", "poll"});
  }

  return {std::move(worker_command_args), std::move(env)};
}

std::tuple<Process, StartupToken> WorkerPool::StartWorkerProcess(
    const Language &language,
    const rpc::WorkerType worker_type,
    const JobID &job_id,
    PopWorkerStatus *status,
    const std::vector<std::string> &dynamic_options,
    const int runtime_env_hash,
    const std::string &serialized_runtime_env_context,
    const rpc::RuntimeEnvInfo &runtime_env_info,
    std::optional<absl::Duration> worker_startup_keep_alive_duration) {
  rpc::JobConfig *job_config = nullptr;
  if (!job_id.IsNil()) {
    auto it = all_jobs_.find(job_id);
    if (it == all_jobs_.end()) {
      RAY_LOG(DEBUG) << "Job config of job " << job_id << " are not local yet.";
      // Will reschedule ready tasks in `NodeManager::HandleJobStarted`.
      *status = PopWorkerStatus::JobConfigMissing;
      process_failed_job_config_missing_++;
      return {Process(), (StartupToken)-1};
    }
    job_config = &it->second;
  }

  auto &state = GetStateForLanguage(language);
  // If we are already starting up too many workers of the same worker type, then return
  // without starting more.
  int starting_workers = 0;
  for (auto &entry : state.worker_processes) {
    if (entry.second.worker_type == worker_type) {
      starting_workers += entry.second.is_pending_registration ? 1 : 0;
    }
  }

  // Here we consider both task workers and I/O workers.
  if (starting_workers >= maximum_startup_concurrency_) {
    // Workers have been started, but not registered. Force start disabled -- returning.
    RAY_LOG(DEBUG) << "Worker not started, exceeding maximum_startup_concurrency("
                   << maximum_startup_concurrency_ << "), " << starting_workers
                   << " workers of language type " << static_cast<int>(language)
                   << " being started and pending registration";
    *status = PopWorkerStatus::TooManyStartingWorkerProcesses;
    process_failed_rate_limited_++;
    return {Process(), (StartupToken)-1};
  }
  // Either there are no workers pending registration or the worker start is being forced.
  RAY_LOG(DEBUG) << "Starting new worker process of language "
                 << rpc::Language_Name(language) << " and type "
                 << rpc::WorkerType_Name(worker_type) << ", current pool has "
                 << state.idle.size() << " workers";

  auto [worker_command_args, env] =
      BuildProcessCommandArgs(language,
                              job_config,
                              worker_type,
                              job_id,
                              dynamic_options,
                              runtime_env_hash,
                              serialized_runtime_env_context,
                              state);

  auto start = std::chrono::high_resolution_clock::now();
  // Start a process and measure the startup time.
  Process proc = StartProcess(worker_command_args, env);
  stats::NumWorkersStarted.Record(1);
  RAY_LOG(INFO) << "Started worker process with pid " << proc.GetId() << ", the token is "
                << worker_startup_token_counter_;
  if (!IsIOWorkerType(worker_type)) {
    AdjustWorkerOomScore(proc.GetId());
  }
  MonitorStartingWorkerProcess(worker_startup_token_counter_, language, worker_type);
  AddWorkerProcess(state,
                   worker_type,
                   proc,
                   start,
                   runtime_env_info,
                   dynamic_options,
                   worker_startup_keep_alive_duration);
  StartupToken worker_startup_token = worker_startup_token_counter_;
  update_worker_startup_token_counter();
  if (IsIOWorkerType(worker_type)) {
    auto &io_worker_state = GetIOWorkerStateFromWorkerType(worker_type, state);
    io_worker_state.num_starting_io_workers++;
  }
  return {proc, worker_startup_token};
}

void WorkerPool::AdjustWorkerOomScore(pid_t pid) const {
#ifdef __linux__
  std::ofstream oom_score_file;
  std::string filename("/proc/" + std::to_string(pid) + "/oom_score_adj");
  oom_score_file.open(filename, std::ofstream::out);
  int oom_score_adj = RayConfig::instance().worker_oom_score_adjustment();
  oom_score_adj = std::max(oom_score_adj, 0);
  oom_score_adj = std::min(oom_score_adj, 1000);
  if (oom_score_file.is_open()) {
    // Adjust worker's OOM score so that the OS prioritizes killing these
    // processes over the raylet.
    oom_score_file << std::to_string(oom_score_adj);
  }
  if (oom_score_file.fail()) {
    RAY_LOG(INFO) << "Failed to set OOM score adjustment for worker with PID " << pid
                  << ", error: " << strerror(errno);
  }
  oom_score_file.close();
#endif
}

void WorkerPool::MonitorStartingWorkerProcess(StartupToken proc_startup_token,
                                              const Language &language,
                                              const rpc::WorkerType worker_type) {
  auto timer = std::make_shared<boost::asio::deadline_timer>(
      *io_service_,
      boost::posix_time::seconds(
          RayConfig::instance().worker_register_timeout_seconds()));
  // Capture timer in lambda to copy it once, so that it can avoid destructing timer.
  timer->async_wait([timer, language, proc_startup_token, worker_type, this](
                        const boost::system::error_code e) mutable {
    // check the error code.
    auto &state = this->GetStateForLanguage(language);
    // Since this process times out to start, remove it from worker_processes
    // to avoid the zombie worker.
    auto it = state.worker_processes.find(proc_startup_token);
    if (it != state.worker_processes.end() && it->second.is_pending_registration) {
      RAY_LOG(ERROR)
          << "Some workers of the worker process(" << it->second.proc.GetId()
          << ") have not registered within the timeout. "
          << (it->second.proc.IsAlive()
                  ? "The process is still alive, probably it's hanging during start."
                  : "The process is dead, probably it crashed during start.");

      if (it->second.proc.IsAlive()) {
        it->second.proc.Kill();
      }

      process_failed_pending_registration_++;
      DeleteRuntimeEnvIfPossible(it->second.runtime_env_info.serialized_runtime_env());
      RemoveWorkerProcess(state, proc_startup_token);
      if (IsIOWorkerType(worker_type)) {
        // Mark the I/O worker as failed.
        auto &io_worker_state = GetIOWorkerStateFromWorkerType(worker_type, state);
        io_worker_state.num_starting_io_workers--;
      }
      // We may have places to start more workers now.
      TryStartIOWorkers(language);
      if (worker_type == rpc::WorkerType::WORKER) {
        TryPendingStartRequests(language);
      }
      starting_worker_timeout_callback_();
    }
  });
}

void WorkerPool::MonitorPopWorkerRequestForRegistration(
    std::shared_ptr<PopWorkerRequest> pop_worker_request) {
  auto timer = std::make_shared<boost::asio::deadline_timer>(
      *io_service_,
      boost::posix_time::seconds(
          RayConfig::instance().worker_register_timeout_seconds()));
  // Capture timer in lambda to copy it once, so that it can avoid destructing timer.
  timer->async_wait([timer, pop_worker_request = std::move(pop_worker_request), this](
                        const boost::system::error_code e) mutable {
    auto &state = GetStateForLanguage(pop_worker_request->language);
    auto &requests = state.pending_registration_requests;
    auto it = std::find(requests.begin(), requests.end(), pop_worker_request);
    if (it != requests.end()) {
      // Pop and fail the task...
      requests.erase(it);
      PopWorkerStatus status = PopWorkerStatus::WorkerPendingRegistration;
      PopWorkerCallbackAsync(pop_worker_request->callback, nullptr, status);
    }
  });
}

Process WorkerPool::StartProcess(const std::vector<std::string> &worker_command_args,
                                 const ProcessEnvironment &env) {
  if (RAY_LOG_ENABLED(DEBUG)) {
    std::string debug_info;
    debug_info.append("Starting worker process with command:");
    for (const auto &arg : worker_command_args) {
      debug_info.append(" ").append(arg);
    }
    debug_info.append(", and the envs:");
    for (const auto &entry : env) {
      debug_info.append(" ")
          .append(entry.first)
          .append(":")
          .append(entry.second)
          .append(",");
    }
    if (!env.empty()) {
      // Erase the last ","
      debug_info.pop_back();
    }
    debug_info.append(".");
    RAY_LOG(DEBUG) << debug_info;
  }

  // Launch the process to create the worker.
  std::error_code ec;
  std::vector<const char *> argv;
  for (const std::string &arg : worker_command_args) {
    argv.push_back(arg.c_str());
  }
  argv.push_back(NULL);

  Process child(argv.data(), io_service_, ec, /*decouple=*/false, env);
  if (!child.IsValid() || ec) {
    // errorcode 24: Too many files. This is caused by ulimit.
    if (ec.value() == 24) {
      RAY_LOG(FATAL) << "Too many workers, failed to create a file. Try setting "
                     << "`ulimit -n <num_files>` then restart Ray.";
    } else {
      // The worker failed to start. This is a fatal error.
      RAY_LOG(FATAL) << "Failed to start worker with return value " << ec << ": "
                     << ec.message();
    }
  }
  return child;
}

Status WorkerPool::GetNextFreePort(int *port) {
  if (!free_ports_) {
    *port = 0;
    return Status::OK();
  }

  // Try up to the current number of ports.
  int current_size = free_ports_->size();
  for (int i = 0; i < current_size; i++) {
    *port = free_ports_->front();
    free_ports_->pop();
    if (CheckPortFree(*port)) {
      return Status::OK();
    }
    // Return to pool to check later.
    free_ports_->push(*port);
  }
  return Status::Invalid(
      "No available ports. Please specify a wider port range using --min-worker-port and "
      "--max-worker-port.");
}

void WorkerPool::MarkPortAsFree(int port) {
  if (free_ports_) {
    RAY_CHECK(port != 0) << "";
    free_ports_->push(port);
  }
}

static bool NeedToEagerInstallRuntimeEnv(const rpc::JobConfig &job_config) {
  if (job_config.has_runtime_env_info() &&
      job_config.runtime_env_info().has_runtime_env_config() &&
      job_config.runtime_env_info().runtime_env_config().eager_install()) {
    auto const &runtime_env = job_config.runtime_env_info().serialized_runtime_env();
    return !IsRuntimeEnvEmpty(runtime_env);
  }
  return false;
}

void WorkerPool::HandleJobStarted(const JobID &job_id, const rpc::JobConfig &job_config) {
  if (all_jobs_.find(job_id) != all_jobs_.end()) {
    RAY_LOG(INFO) << "Job " << job_id << " already started in worker pool.";
    return;
  }
  all_jobs_[job_id] = job_config;
  if (NeedToEagerInstallRuntimeEnv(job_config)) {
    auto const &runtime_env = job_config.runtime_env_info().serialized_runtime_env();
    auto const &runtime_env_config = job_config.runtime_env_info().runtime_env_config();
    // NOTE: Technically `HandleJobStarted` isn't idempotent because we'll
    // increment the ref count multiple times. This is fine because
    // `HandleJobFinished` will also decrement the ref count multiple times.
    RAY_LOG(INFO) << "[Eagerly] Start install runtime environment for job " << job_id
                  << ".";
    RAY_LOG(DEBUG) << "Runtime env for job " << job_id << ": " << runtime_env;
    GetOrCreateRuntimeEnv(
        runtime_env,
        runtime_env_config,
        job_id,
        [job_id](bool successful,
                 const std::string &serialized_runtime_env_context,
                 const std::string &setup_error_message) {
          if (successful) {
            RAY_LOG(INFO) << "[Eagerly] Create runtime env successful for job " << job_id
                          << ".";
          } else {
            RAY_LOG(WARNING) << "[Eagerly] Couldn't create a runtime environment for job "
                             << job_id << ". Error message: " << setup_error_message;
          }
        });
  }
}

void WorkerPool::HandleJobFinished(const JobID &job_id) {
  // Currently we don't erase the job from `all_jobs_` , as a workaround for
  // https://github.com/ray-project/ray/issues/11437.
  // unfinished_jobs_.erase(job_id);
  auto job_config = GetJobConfig(job_id);
  RAY_CHECK(job_config);
  // Check eager install here because we only add URI reference when runtime
  // env install really happens.
  if (NeedToEagerInstallRuntimeEnv(*job_config)) {
    DeleteRuntimeEnvIfPossible(job_config->runtime_env_info().serialized_runtime_env());
  }
  finished_jobs_.insert(job_id);
}

boost::optional<const rpc::JobConfig &> WorkerPool::GetJobConfig(
    const JobID &job_id) const {
  auto iter = all_jobs_.find(job_id);
  return iter == all_jobs_.end() ? boost::none
                                 : boost::optional<const rpc::JobConfig &>(iter->second);
}

// TODO(hjiang): In the next integration PR, worker should have port assigned and no
// [send_reply_callback]. Should delete this overload.
Status WorkerPool::RegisterWorker(const std::shared_ptr<WorkerInterface> &worker,
                                  pid_t pid,
                                  StartupToken worker_startup_token,
                                  std::function<void(Status, int)> send_reply_callback) {
  RAY_CHECK(worker);
  auto &state = GetStateForLanguage(worker->GetLanguage());
  auto it = state.worker_processes.find(worker_startup_token);
  if (it == state.worker_processes.end()) {
    RAY_LOG(WARNING) << "Received a register request from an unknown token: "
                     << worker_startup_token;
    Status status = Status::Invalid("Unknown worker");
    send_reply_callback(status, /*port=*/0);
    return status;
  }

  auto process = Process::FromPid(pid);
  worker->SetProcess(process);

  // The port that this worker's gRPC server should listen on. 0 if the worker
  // should bind on a random port.
  int port = 0;
  Status status = GetNextFreePort(&port);
  if (!status.ok()) {
    send_reply_callback(status, /*port=*/0);
    return status;
  }
  auto &starting_process_info = it->second;
  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      end - starting_process_info.start_time);
  STATS_worker_register_time_ms.Record(duration.count());
  RAY_LOG(DEBUG) << "Registering worker " << worker->WorkerId() << " with pid " << pid
                 << ", port: " << port << ", register cost: " << duration.count()
                 << ", worker_type: " << rpc::WorkerType_Name(worker->GetWorkerType())
                 << ", startup token: " << worker_startup_token;
  worker->SetAssignedPort(port);

  state.registered_workers.insert(worker);

  // Send the reply immediately for worker registrations.
  send_reply_callback(Status::OK(), port);
  return Status::OK();
}

Status WorkerPool::RegisterWorker(const std::shared_ptr<WorkerInterface> &worker,
                                  pid_t pid,
                                  StartupToken worker_startup_token) {
  RAY_CHECK(worker);
  auto &state = GetStateForLanguage(worker->GetLanguage());
  auto it = state.worker_processes.find(worker_startup_token);
  if (it == state.worker_processes.end()) {
    RAY_LOG(WARNING) << "Received a register request from an unknown token: "
                     << worker_startup_token;
    return Status::Invalid("Unknown worker");
  }

  auto process = Process::FromPid(pid);
  worker->SetProcess(process);

  auto &starting_process_info = it->second;
  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      end - starting_process_info.start_time);

  // TODO(hjiang): Add tag to indicate whether port has been assigned beforehand.
  STATS_worker_register_time_ms.Record(duration.count());
  RAY_LOG(DEBUG) << "Registering worker " << worker->WorkerId() << " with pid " << pid
                 << ", register cost: " << duration.count()
                 << ", worker_type: " << rpc::WorkerType_Name(worker->GetWorkerType())
                 << ", startup token: " << worker_startup_token;

  state.registered_workers.insert(worker);
  return Status::OK();
}

void WorkerPool::OnWorkerStarted(const std::shared_ptr<WorkerInterface> &worker) {
  auto &state = GetStateForLanguage(worker->GetLanguage());
  const StartupToken worker_startup_token = worker->GetStartupToken();
  const auto &worker_type = worker->GetWorkerType();

  auto it = state.worker_processes.find(worker_startup_token);
  if (it != state.worker_processes.end()) {
    it->second.is_pending_registration = false;
    // We may have slots to start more workers now.
    TryStartIOWorkers(worker->GetLanguage());
  }
  if (IsIOWorkerType(worker_type)) {
    auto &io_worker_state = GetIOWorkerStateFromWorkerType(worker_type, state);
    io_worker_state.started_io_workers.insert(worker);
    io_worker_state.num_starting_io_workers--;
  }

  // This is a workaround to finish driver registration after all initial workers are
  // registered to Raylet if and only if Raylet is started by a Python driver and the
  // job config is not set in `ray.init(...)`.
  if (worker_type == rpc::WorkerType::WORKER &&
      worker->GetLanguage() == Language::PYTHON) {
    if (++first_job_registered_python_worker_count_ ==
        first_job_driver_wait_num_python_workers_) {
      if (first_job_send_register_client_reply_to_driver_) {
        first_job_send_register_client_reply_to_driver_();
        first_job_send_register_client_reply_to_driver_ = nullptr;
      }
    }
  }
}

void WorkerPool::ExecuteOnPrestartWorkersStarted(std::function<void()> callback) {
  bool prestart = RayConfig::instance().prestart_worker_first_driver() ||
                  RayConfig::instance().enable_worker_prestart();
  if (first_job_registered_ ||
      first_job_registered_python_worker_count_ >=  // Don't wait if prestart is completed
          first_job_driver_wait_num_python_workers_ ||
      !prestart) {  // Don't wait if prestart is disabled
    callback();
    return;
  }
  first_job_registered_ = true;
  RAY_CHECK(!first_job_send_register_client_reply_to_driver_);
  first_job_send_register_client_reply_to_driver_ = std::move(callback);
}

Status WorkerPool::RegisterDriver(const std::shared_ptr<WorkerInterface> &driver,
                                  const rpc::JobConfig &job_config,
                                  std::function<void(Status, int)> send_reply_callback) {
  int port;
  RAY_CHECK(!driver->GetAssignedTaskId().IsNil());
  Status status = GetNextFreePort(&port);
  if (!status.ok()) {
    send_reply_callback(status, /*port=*/0);
    return status;
  }
  driver->SetAssignedPort(port);
  auto &state = GetStateForLanguage(driver->GetLanguage());
  state.registered_drivers.insert(std::move(driver));
  const auto job_id = driver->GetAssignedJobId();
  HandleJobStarted(job_id, job_config);

  if (driver->GetLanguage() == Language::JAVA) {
    send_reply_callback(Status::OK(), port);
  } else {
    if (!first_job_registered_ && RayConfig::instance().prestart_worker_first_driver() &&
        !RayConfig::instance().enable_worker_prestart()) {
      RAY_LOG(DEBUG) << "PrestartDefaultCpuWorkers " << num_prestart_python_workers;
      rpc::TaskSpec rpc_task_spec;
      rpc_task_spec.set_language(Language::PYTHON);
      rpc_task_spec.mutable_runtime_env_info()->set_serialized_runtime_env("{}");

      TaskSpecification task_spec{std::move(rpc_task_spec)};
      PrestartWorkersInternal(task_spec, num_prestart_python_workers);
    }

    // Invoke the `send_reply_callback` later to only finish driver
    // registration after all prestarted workers are registered to Raylet.
    // NOTE(clarng): prestart is only for python workers.
    ExecuteOnPrestartWorkersStarted(
        [send_reply_callback = std::move(send_reply_callback), port]() {
          send_reply_callback(Status::OK(), port);
        });
  }
  return Status::OK();
}

std::shared_ptr<WorkerInterface> WorkerPool::GetRegisteredWorker(
    const WorkerID &worker_id) const {
  for (const auto &entry : states_by_lang_) {
    auto worker = GetWorker(entry.second.registered_workers, worker_id);
    if (worker != nullptr) {
      return worker;
    }
  }
  return nullptr;
}

std::shared_ptr<WorkerInterface> WorkerPool::GetRegisteredWorker(
    const std::shared_ptr<ClientConnection> &connection) const {
  for (const auto &entry : states_by_lang_) {
    auto worker = GetWorker(entry.second.registered_workers, connection);
    if (worker != nullptr) {
      return worker;
    }
  }
  return nullptr;
}

std::shared_ptr<WorkerInterface> WorkerPool::GetRegisteredDriver(
    const WorkerID &worker_id) const {
  for (const auto &entry : states_by_lang_) {
    auto driver = GetWorker(entry.second.registered_drivers, worker_id);
    if (driver != nullptr) {
      return driver;
    }
  }
  return nullptr;
}

std::shared_ptr<WorkerInterface> WorkerPool::GetRegisteredDriver(
    const std::shared_ptr<ClientConnection> &connection) const {
  for (const auto &entry : states_by_lang_) {
    auto driver = GetWorker(entry.second.registered_drivers, connection);
    if (driver != nullptr) {
      return driver;
    }
  }
  return nullptr;
}

void WorkerPool::PushSpillWorker(const std::shared_ptr<WorkerInterface> &worker) {
  PushIOWorkerInternal(worker, rpc::WorkerType::SPILL_WORKER);
}

void WorkerPool::PopSpillWorker(
    std::function<void(std::shared_ptr<WorkerInterface>)> callback) {
  PopIOWorkerInternal(rpc::WorkerType::SPILL_WORKER, callback);
}

void WorkerPool::PushRestoreWorker(const std::shared_ptr<WorkerInterface> &worker) {
  PushIOWorkerInternal(worker, rpc::WorkerType::RESTORE_WORKER);
}

void WorkerPool::PopRestoreWorker(
    std::function<void(std::shared_ptr<WorkerInterface>)> callback) {
  PopIOWorkerInternal(rpc::WorkerType::RESTORE_WORKER, callback);
}

void WorkerPool::PushIOWorkerInternal(const std::shared_ptr<WorkerInterface> &worker,
                                      const rpc::WorkerType &worker_type) {
  RAY_CHECK(IsIOWorkerType(worker->GetWorkerType()));
  auto &state = GetStateForLanguage(Language::PYTHON);
  auto &io_worker_state = GetIOWorkerStateFromWorkerType(worker_type, state);

  if (!io_worker_state.started_io_workers.count(worker)) {
    RAY_LOG(DEBUG)
        << "The IO worker has failed. Skip pushing it to the worker pool. Worker type: "
        << rpc::WorkerType_Name(worker_type) << ", worker id: " << worker->WorkerId();
    return;
  }

  RAY_LOG(DEBUG) << "Pushing an IO worker to the worker pool. Worker type: "
                 << rpc::WorkerType_Name(worker_type)
                 << ", worker id: " << worker->WorkerId();
  if (io_worker_state.pending_io_tasks.empty()) {
    io_worker_state.idle_io_workers.emplace(worker);
  } else {
    auto callback = io_worker_state.pending_io_tasks.front();
    io_worker_state.pending_io_tasks.pop();
    callback(worker);
  }
}

void WorkerPool::PopIOWorkerInternal(
    const rpc::WorkerType &worker_type,
    std::function<void(std::shared_ptr<WorkerInterface>)> callback) {
  auto &state = GetStateForLanguage(Language::PYTHON);
  auto &io_worker_state = GetIOWorkerStateFromWorkerType(worker_type, state);

  if (io_worker_state.idle_io_workers.empty()) {
    // We must fill the pending task first, because 'TryStartIOWorkers' will
    // start I/O workers according to the number of pending tasks.
    io_worker_state.pending_io_tasks.push(callback);
    RAY_LOG(DEBUG) << "There are no idle workers, try starting a new one. Try starting a "
                      "new one. Worker type: "
                   << rpc::WorkerType_Name(worker_type);
    TryStartIOWorkers(Language::PYTHON, worker_type);
  } else {
    const auto it = io_worker_state.idle_io_workers.begin();
    auto io_worker = *it;
    io_worker_state.idle_io_workers.erase(it);
    RAY_LOG(DEBUG) << "Popped an IO worker. Worker type: "
                   << rpc::WorkerType_Name(worker_type)
                   << ", worker ID: " << io_worker->WorkerId();
    callback(io_worker);
  }
}

void WorkerPool::PushDeleteWorker(const std::shared_ptr<WorkerInterface> &worker) {
  RAY_CHECK(IsIOWorkerType(worker->GetWorkerType()));
  if (worker->GetWorkerType() == rpc::WorkerType::RESTORE_WORKER) {
    PushRestoreWorker(worker);
  } else {
    PushSpillWorker(worker);
  }
}

void WorkerPool::PopDeleteWorker(
    std::function<void(std::shared_ptr<WorkerInterface>)> callback) {
  auto &state = GetStateForLanguage(Language::PYTHON);
  // Choose an I/O worker with more idle workers.
  size_t num_spill_idle_workers = state.spill_io_worker_state.idle_io_workers.size();
  size_t num_restore_idle_workers = state.restore_io_worker_state.idle_io_workers.size();

  if (num_restore_idle_workers < num_spill_idle_workers) {
    PopSpillWorker(callback);
  } else {
    PopRestoreWorker(callback);
  }
}

void WorkerPool::PushWorker(const std::shared_ptr<WorkerInterface> &worker) {
  // Since the worker is now idle, unset its assigned task ID.
  RAY_CHECK(worker->GetAssignedTaskId().IsNil())
      << "Idle workers cannot have an assigned task ID";

  // Find a task that this worker can fit. If there's none, put it in the idle pool.
  // First find in pending_registration_requests, then in pending_start_requests.
  std::shared_ptr<PopWorkerRequest> pop_worker_request = nullptr;
  auto &state = GetStateForLanguage(worker->GetLanguage());
  {
    auto it = std::find_if(
        state.pending_registration_requests.begin(),
        state.pending_registration_requests.end(),
        [this, &worker](const std::shared_ptr<PopWorkerRequest> &pop_worker_request) {
          return WorkerFitsForTask(*worker, *pop_worker_request) ==
                 WorkerUnfitForTaskReason::NONE;
        });
    if (it != state.pending_registration_requests.end()) {
      pop_worker_request = *it;
      state.pending_registration_requests.erase(it);
    }
  }
  if (!pop_worker_request) {
    auto it = std::find_if(
        state.pending_start_requests.begin(),
        state.pending_start_requests.end(),
        [this, &worker](const std::shared_ptr<PopWorkerRequest> &pop_worker_request) {
          return WorkerFitsForTask(*worker, *pop_worker_request) ==
                 WorkerUnfitForTaskReason::NONE;
        });
    if (it != state.pending_start_requests.end()) {
      pop_worker_request = *it;
      state.pending_start_requests.erase(it);
    }
  }

  if (pop_worker_request) {
    bool used = pop_worker_request->callback(worker, PopWorkerStatus::OK, "");
    if (!used) {
      // Retry PushWorker. Maybe it can be used by other tasks.
      // Can we have tail call optimization for this? :)
      return PushWorker(worker);
    }
  } else {
    // Worker pushed without suiting any pending request. Put to idle pool with
    // keep_alive_until.
    state.idle.insert(worker);
    auto now = get_time_();
    absl::Time keep_alive_until =
        now +
        absl::Milliseconds(RayConfig::instance().idle_worker_killing_time_threshold_ms());
    if (worker->GetAssignedTaskTime() == absl::Time()) {
      // Newly registered worker. Respect worker_startup_keep_alive_duration if any.
      auto it = state.worker_processes.find(worker->GetStartupToken());
      if (it != state.worker_processes.end()) {
        const auto &keep_alive_duration = it->second.worker_startup_keep_alive_duration;
        if (keep_alive_duration.has_value()) {
          keep_alive_until = std::max(keep_alive_until, now + *keep_alive_duration);
        }
      }

      // If the worker never held any tasks, then we should consider it first when
      // choosing which idle workers to kill because it is not warmed up and is slower
      // than those workers who served tasks before.
      // See https://github.com/ray-project/ray/pull/36766
      //
      // Also, we set keep_alive_until w.r.t. worker_startup_keep_alive_duration.
      idle_of_all_languages_.emplace_front(IdleWorkerEntry{worker, keep_alive_until});
    } else {
      idle_of_all_languages_.emplace_back(IdleWorkerEntry{worker, keep_alive_until});
    }
  }
  // We either have an idle worker or a slot to start a new worker.
  if (worker->GetWorkerType() == rpc::WorkerType::WORKER) {
    TryPendingStartRequests(worker->GetLanguage());
  }
}

void WorkerPool::TryKillingIdleWorkers() {
  const absl::Time now = get_time_();

  // Filter out all idle workers that are already dead and/or associated with
  // jobs that have already finished.
  int64_t num_killable_idle_workers = 0;
  auto worker_killable = [now](const IdleWorkerEntry &entry) -> bool {
    return entry.keep_alive_until < now;
  };

  // First, kill must-kill workers: dead ones, job finished ones. Also calculate killable
  // worker count.
  for (auto it = idle_of_all_languages_.begin(); it != idle_of_all_languages_.end();) {
    if (it->worker->IsDead()) {
      it = idle_of_all_languages_.erase(it);
      continue;
    }

    const auto &job_id = it->worker->GetAssignedJobId();
    if (finished_jobs_.contains(job_id)) {
      // The job has finished, so we should kill the worker immediately.
      KillIdleWorker(*it);
      it = idle_of_all_languages_.erase(it);
    } else {
      if (worker_killable(*it)) {
        // The job has not yet finished and the worker has been idle for longer
        // than the timeout.
        num_killable_idle_workers++;
      }
      it++;
    }
  }

  // Compute the soft limit for the number of idle workers to keep around.
  // This assumes the common case where each task requires 1 CPU.
  const auto num_desired_idle_workers = get_num_cpus_available_();
  RAY_LOG(DEBUG) << "Idle workers: " << idle_of_all_languages_.size()
                 << ", idle workers that are eligible to kill: "
                 << num_killable_idle_workers
                 << ", num desired workers : " << num_desired_idle_workers;

  // Iterate through the list and try to kill enough workers so that we are at
  // the soft limit.
  auto it = idle_of_all_languages_.begin();
  while (num_killable_idle_workers > num_desired_idle_workers &&
         it != idle_of_all_languages_.end()) {
    if (worker_killable(*it)) {
      RAY_LOG(DEBUG) << "Number of idle workers " << num_killable_idle_workers
                     << " is larger than the number of desired workers "
                     << num_desired_idle_workers << " killing idle worker with PID "
                     << it->worker->GetProcess().GetId();
      KillIdleWorker(*it);
      it = idle_of_all_languages_.erase(it);
      num_killable_idle_workers--;
    } else {
      it++;
    }
  }
}

void WorkerPool::KillIdleWorker(const IdleWorkerEntry &entry) {
  const auto &idle_worker = entry.worker;
  // To avoid object lost issue caused by forcibly killing, send an RPC request to the
  // worker to allow it to do cleanup before exiting. We kill it anyway if the driver
  // is already exited.
  RAY_LOG(DEBUG) << "Sending exit message to worker " << idle_worker->WorkerId();
  // Register the worker to pending exit so that we can correctly calculate the
  // running_size.
  // This also means that there's an inflight `Exit` RPC request to the worker.
  pending_exit_idle_workers_.emplace(idle_worker->WorkerId(), idle_worker);
  auto rpc_client = idle_worker->rpc_client();
  RAY_CHECK(rpc_client);
  rpc::ExitRequest request;
  const auto &job_id = idle_worker->GetAssignedJobId();
  if (finished_jobs_.contains(job_id) && idle_worker->GetRootDetachedActorId().IsNil()) {
    RAY_LOG(INFO) << "Force exiting worker whose job has exited "
                  << idle_worker->WorkerId();
    request.set_force_exit(true);
  }
  rpc_client->Exit(
      request, [this, entry](const ray::Status &status, const rpc::ExitReply &r) {
        const auto &idle_worker = entry.worker;

        RAY_CHECK(pending_exit_idle_workers_.erase(idle_worker->WorkerId()));
        if (!status.ok()) {
          RAY_LOG(ERROR) << "Failed to send exit request: " << status.ToString();
        }

        // In case of failed to send request, we remove it from pool as well
        // TODO(iycheng): We should handle the grpc failure in better way.
        if (!status.ok() || r.success()) {
          RAY_LOG(DEBUG) << "Removed worker " << idle_worker->WorkerId();
          auto &worker_state = GetStateForLanguage(idle_worker->GetLanguage());
          // If we could kill the worker properly, we remove them from the idle
          // pool.
          RemoveWorker(worker_state.idle, idle_worker);
          // We always mark the worker as dead.
          // If the worker is not idle at this moment, we'd want to mark it as dead
          // so it won't be reused later.
          if (!idle_worker->IsDead()) {
            idle_worker->MarkDead();
          }
        } else {
          RAY_LOG(DEBUG) << "Failed to remove worker " << idle_worker->WorkerId();
          // We re-insert the idle worker to the back of the queue if it fails to
          // kill the worker (e.g., when the worker owns the object). Without this,
          // if the first N workers own objects, it can't kill idle workers that are
          // >= N+1.
          idle_of_all_languages_.push_back(entry);
        }
      });
}

WorkerUnfitForTaskReason WorkerPool::WorkerFitsForTask(
    const WorkerInterface &worker, const PopWorkerRequest &pop_worker_request) const {
  if (worker.IsDead()) {
    return WorkerUnfitForTaskReason::OTHERS;
  }
  // These workers are exiting. So skip them.
  if (pending_exit_idle_workers_.contains(worker.WorkerId())) {
    return WorkerUnfitForTaskReason::OTHERS;
  }
  if (worker.GetLanguage() != pop_worker_request.language) {
    return WorkerUnfitForTaskReason::OTHERS;
  }
  if (worker.GetWorkerType() != pop_worker_request.worker_type) {
    return WorkerUnfitForTaskReason::OTHERS;
  }

  // For scheduling requests with a root detached actor ID, ensure that either the
  // worker has _no_ detached actor ID or it matches the request.
  // NOTE(edoakes): the job ID for a worker with no detached actor ID must still match,
  // which is checked below. The pop_worker_request for a task rooted in a detached
  // actor will have the job ID of the job that created the detached actor.
  if (!pop_worker_request.root_detached_actor_id.IsNil() &&
      !worker.GetRootDetachedActorId().IsNil() &&
      pop_worker_request.root_detached_actor_id != worker.GetRootDetachedActorId()) {
    return WorkerUnfitForTaskReason::ROOT_MISMATCH;
  }

  // Only consider workers that haven't been assigned to a job yet or have been assigned
  // to the requested job.
  const auto worker_job_id = worker.GetAssignedJobId();
  if (!worker_job_id.IsNil() && pop_worker_request.job_id != worker_job_id) {
    return WorkerUnfitForTaskReason::ROOT_MISMATCH;
  }

  // If the request asks for a is_gpu, and the worker is assigned a different is_gpu,
  // then skip it.
  if (!OptionalsMatchOrEitherEmpty(pop_worker_request.is_gpu, worker.GetIsGpu())) {
    return WorkerUnfitForTaskReason::OTHERS;
  }
  // If the request asks for a is_actor_worker, and the worker is assigned a different
  // is_actor_worker, then skip it.
  if (!OptionalsMatchOrEitherEmpty(pop_worker_request.is_actor_worker,
                                   worker.GetIsActorWorker())) {
    return WorkerUnfitForTaskReason::OTHERS;
  }
  // Skip workers with a mismatched runtime_env.
  // Even if the task doesn't have a runtime_env specified, we cannot schedule it to a
  // worker with a runtime_env because the task is expected to run in the base
  // environment.
  if (worker.GetRuntimeEnvHash() != pop_worker_request.runtime_env_hash) {
    return WorkerUnfitForTaskReason::RUNTIME_ENV_MISMATCH;
  }
  // Skip if the dynamic_options doesn't match.
  if (LookupWorkerDynamicOptions(worker.GetStartupToken()) !=
      pop_worker_request.dynamic_options) {
    return WorkerUnfitForTaskReason::DYNAMIC_OPTIONS_MISMATCH;
  }
  return WorkerUnfitForTaskReason::NONE;
}

void WorkerPool::StartNewWorker(
    const std::shared_ptr<PopWorkerRequest> &pop_worker_request) {
  auto start_worker_process_fn = [this](
                                     std::shared_ptr<PopWorkerRequest> pop_worker_request,
                                     const std::string &serialized_runtime_env_context) {
    auto &state = GetStateForLanguage(pop_worker_request->language);
    const std::string &serialized_runtime_env =
        pop_worker_request->runtime_env_info.serialized_runtime_env();

    PopWorkerStatus status = PopWorkerStatus::OK;
    auto [proc, startup_token] =
        StartWorkerProcess(pop_worker_request->language,
                           pop_worker_request->worker_type,
                           pop_worker_request->job_id,
                           &status,
                           pop_worker_request->dynamic_options,
                           pop_worker_request->runtime_env_hash,
                           serialized_runtime_env_context,
                           pop_worker_request->runtime_env_info,
                           pop_worker_request->worker_startup_keep_alive_duration);
    if (status == PopWorkerStatus::OK) {
      RAY_CHECK(proc.IsValid());
      WarnAboutSize();
      state.pending_registration_requests.emplace_back(pop_worker_request);
      MonitorPopWorkerRequestForRegistration(pop_worker_request);
    } else if (status == PopWorkerStatus::TooManyStartingWorkerProcesses) {
      // TODO(jjyao) As an optimization, we don't need to delete the runtime env
      // but reuse it the next time we retry the request.
      DeleteRuntimeEnvIfPossible(serialized_runtime_env);
      state.pending_start_requests.emplace_back(std::move(pop_worker_request));
    } else {
      DeleteRuntimeEnvIfPossible(serialized_runtime_env);
      PopWorkerCallbackAsync(std::move(pop_worker_request->callback), nullptr, status);
    }
  };

  const std::string &serialized_runtime_env =
      pop_worker_request->runtime_env_info.serialized_runtime_env();

  if (!IsRuntimeEnvEmpty(serialized_runtime_env)) {
    // create runtime env.
    GetOrCreateRuntimeEnv(
        serialized_runtime_env,
        pop_worker_request->runtime_env_info.runtime_env_config(),
        pop_worker_request->job_id,
        [this, start_worker_process_fn, pop_worker_request](
            bool successful,
            const std::string &serialized_runtime_env_context,
            const std::string &setup_error_message) {
          if (successful) {
            start_worker_process_fn(pop_worker_request, serialized_runtime_env_context);
          } else {
            process_failed_runtime_env_setup_failed_++;
            pop_worker_request->callback(
                nullptr,
                PopWorkerStatus::RuntimeEnvCreationFailed,
                /*runtime_env_setup_error_message*/ setup_error_message);
          }
        });
  } else {
    start_worker_process_fn(pop_worker_request, "");
  }
}

void WorkerPool::PopWorker(const TaskSpecification &task_spec,
                           const PopWorkerCallback &callback) {
  RAY_LOG(DEBUG) << "Pop worker for task " << task_spec.TaskId() << " task name "
                 << task_spec.FunctionDescriptor()->ToString();
  // Code path of actor task.
  RAY_CHECK(!task_spec.IsActorTask()) << "Direct call shouldn't reach here.";

  auto pop_worker_request = std::make_shared<PopWorkerRequest>(
      task_spec.GetLanguage(),
      rpc::WorkerType::WORKER,
      task_spec.JobId(),
      task_spec.RootDetachedActorId(),
      /*is_gpu=*/task_spec.GetRequiredResources().Get(scheduling::ResourceID::GPU()) > 0,
      /*is_actor_worker=*/task_spec.IsActorCreationTask(),
      task_spec.RuntimeEnvInfo(),
      task_spec.GetRuntimeEnvHash(),
      task_spec.DynamicWorkerOptionsOrEmpty(),
      /*worker_startup_keep_alive_duration=*/std::nullopt,
      [this, task_spec, callback](
          const std::shared_ptr<WorkerInterface> &worker,
          PopWorkerStatus status,
          const std::string &runtime_env_setup_error_message) -> bool {
        // We got a worker suitable for the task. Now let's check if the task is still
        // executable.
        if (worker && finished_jobs_.contains(task_spec.JobId()) &&
            task_spec.RootDetachedActorId().IsNil()) {
          // When a job finishes, node manager will kill leased workers one time
          // and worker pool will kill idle workers periodically.
          // The current worker is already removed from the idle workers
          // but hasn't been added to the leased workers since the callback is not called
          // yet. We shouldn't add this worker to the leased workers since killing leased
          // workers for this finished job may already happen and won't happen again (this
          // is one time) so it will cause a process leak. Instead we fail the PopWorker
          // and add the worker back to the idle workers so it can be killed later.
          RAY_CHECK(status == PopWorkerStatus::OK);
          callback(nullptr, PopWorkerStatus::JobFinished, "");
          // Not used
          return false;
        }
        return callback(worker, status, runtime_env_setup_error_message);
      });
  PopWorker(std::move(pop_worker_request));
}

std::shared_ptr<WorkerInterface> WorkerPool::FindAndPopIdleWorker(
    const PopWorkerRequest &pop_worker_request) {
  absl::flat_hash_map<WorkerUnfitForTaskReason, size_t> skip_reason_count;

  auto worker_fits_for_task_fn = [this, &pop_worker_request, &skip_reason_count](
                                     const IdleWorkerEntry &entry) -> bool {
    WorkerUnfitForTaskReason reason =
        WorkerFitsForTask(*entry.worker, pop_worker_request);
    if (reason == WorkerUnfitForTaskReason::NONE) {
      return true;
    }
    skip_reason_count[reason]++;
    if (reason == WorkerUnfitForTaskReason::DYNAMIC_OPTIONS_MISMATCH) {
      stats::NumCachedWorkersSkippedDynamicOptionsMismatch.Record(1);
    } else if (reason == WorkerUnfitForTaskReason::RUNTIME_ENV_MISMATCH) {
      stats::NumCachedWorkersSkippedRuntimeEnvironmentMismatch.Record(1);
    } else if (reason == WorkerUnfitForTaskReason::ROOT_MISMATCH) {
      stats::NumCachedWorkersSkippedJobMismatch.Record(1);
    }
    return false;
  };
  auto &state = GetStateForLanguage(pop_worker_request.language);
  auto worker_it = std::find_if(idle_of_all_languages_.rbegin(),
                                idle_of_all_languages_.rend(),
                                worker_fits_for_task_fn);
  if (worker_it == idle_of_all_languages_.rend()) {
    RAY_LOG(DEBUG) << "No cached worker, cached workers skipped due to "
                   << debug_string(skip_reason_count);
    return nullptr;
  }

  state.idle.erase(worker_it->worker);
  // We can't erase a reverse_iterator.
  auto lit = worker_it.base();
  lit--;
  std::shared_ptr<WorkerInterface> worker = std::move(lit->worker);
  idle_of_all_languages_.erase(lit);

  // Assigned workers should always match the request's job_id
  // *except* if the task originates from a detached actor.
  RAY_CHECK(worker->GetAssignedJobId().IsNil() ||
            worker->GetAssignedJobId() == pop_worker_request.job_id ||
            !pop_worker_request.root_detached_actor_id.IsNil());
  return worker;
}

void WorkerPool::PopWorker(std::shared_ptr<PopWorkerRequest> pop_worker_request) {
  // If there's an idle worker that fits the task, use it.
  // Else, start a new worker.
  auto worker = FindAndPopIdleWorker(*pop_worker_request);
  if (worker == nullptr) {
    StartNewWorker(pop_worker_request);
    return;
  }
  RAY_CHECK(worker->GetAssignedJobId().IsNil() ||
            worker->GetAssignedJobId() == pop_worker_request->job_id);
  stats::NumWorkersStartedFromCache.Record(1);
  PopWorkerCallbackAsync(pop_worker_request->callback, worker, PopWorkerStatus::OK);
}

void WorkerPool::PrestartWorkers(const TaskSpecification &task_spec,
                                 int64_t backlog_size) {
  int64_t num_available_cpus = get_num_cpus_available_();
  // Code path of task that needs a dedicated worker.
  RAY_LOG(DEBUG) << "PrestartWorkers, num_available_cpus " << num_available_cpus
                 << " backlog_size " << backlog_size << " task spec "
                 << task_spec.DebugString() << " has runtime env "
                 << task_spec.HasRuntimeEnv();
  if ((task_spec.IsActorCreationTask() && !task_spec.DynamicWorkerOptions().empty()) ||
      task_spec.GetLanguage() != ray::Language::PYTHON) {
    return;  // Not handled.
  }

  auto &state = GetStateForLanguage(task_spec.GetLanguage());
  // The number of available workers that can be used for this task spec.
  int num_usable_workers = state.idle.size();
  for (auto &entry : state.worker_processes) {
    num_usable_workers += entry.second.is_pending_registration ? 1 : 0;
  }
  // Some existing workers may be holding less than 1 CPU each, so we should
  // start as many workers as needed to fill up the remaining CPUs.
  auto desired_usable_workers = std::min<int64_t>(num_available_cpus, backlog_size);
  if (num_usable_workers < desired_usable_workers) {
    // Account for workers that are idle or already starting.
    int64_t num_needed = desired_usable_workers - num_usable_workers;
    RAY_LOG(DEBUG) << "Prestarting " << num_needed << " workers given task backlog size "
                   << backlog_size << " and available CPUs " << num_available_cpus
                   << " num idle workers " << state.idle.size()
                   << " num registered workers " << state.registered_workers.size();
    PrestartWorkersInternal(task_spec, num_needed);
  }
}

void WorkerPool::PrestartWorkersInternal(const TaskSpecification &task_spec,
                                         int64_t num_needed) {
  RAY_LOG(DEBUG) << "PrestartWorkers " << num_needed;
  for (int ii = 0; ii < num_needed; ++ii) {
    // Prestart worker with no runtime env.
    if (IsRuntimeEnvEmpty(task_spec.SerializedRuntimeEnv())) {
      PopWorkerStatus status;
      StartWorkerProcess(
          task_spec.GetLanguage(), rpc::WorkerType::WORKER, task_spec.JobId(), &status);
      continue;
    }

    // Prestart worker with runtime env.
    GetOrCreateRuntimeEnv(
        task_spec.SerializedRuntimeEnv(),
        task_spec.RuntimeEnvConfig(),
        task_spec.JobId(),
        [this, task_spec = task_spec](bool successful,
                                      const std::string &serialized_runtime_env_context,
                                      const std::string &setup_error_message) {
          if (!successful) {
            RAY_LOG(ERROR) << "Fails to create or get runtime env "
                           << setup_error_message;
            return;
          }
          PopWorkerStatus status;
          StartWorkerProcess(task_spec.GetLanguage(),
                             rpc::WorkerType::WORKER,
                             task_spec.JobId(),
                             &status,
                             /*dynamic_options=*/{},
                             task_spec.GetRuntimeEnvHash(),
                             serialized_runtime_env_context,
                             task_spec.RuntimeEnvInfo());
        });
  }
}

void WorkerPool::DisconnectWorker(const std::shared_ptr<WorkerInterface> &worker,
                                  rpc::WorkerExitType disconnect_type) {
  MarkPortAsFree(worker->AssignedPort());
  auto &state = GetStateForLanguage(worker->GetLanguage());
  auto it = state.worker_processes.find(worker->GetStartupToken());
  if (it != state.worker_processes.end()) {
    const auto serialized_runtime_env =
        it->second.runtime_env_info.serialized_runtime_env();
    if (it->second.is_pending_registration) {
      // Worker is either starting or started,
      // if it's not started, we should remove it from starting.
      it->second.is_pending_registration = false;
      if (worker->GetWorkerType() == rpc::WorkerType::WORKER) {
        // This may add new workers to state.worker_processes
        // and invalidate the iterator, do not use `it`
        // after this call.
        TryPendingStartRequests(worker->GetLanguage());
      }
    }

    DeleteRuntimeEnvIfPossible(serialized_runtime_env);
    RemoveWorkerProcess(state, worker->GetStartupToken());
  }
  RAY_CHECK(RemoveWorker(state.registered_workers, worker));

  if (IsIOWorkerType(worker->GetWorkerType())) {
    auto &io_worker_state =
        GetIOWorkerStateFromWorkerType(worker->GetWorkerType(), state);
    if (!RemoveWorker(io_worker_state.started_io_workers, worker)) {
      // IO worker is either starting or started,
      // if it's not started, we should remove it from starting.
      io_worker_state.num_starting_io_workers--;
    }
    RemoveWorker(io_worker_state.idle_io_workers, worker);
    return;
  }

  for (auto idle_worker_iter = idle_of_all_languages_.begin();
       idle_worker_iter != idle_of_all_languages_.end();
       idle_worker_iter++) {
    if (idle_worker_iter->worker == worker) {
      idle_of_all_languages_.erase(idle_worker_iter);
      break;
    }
  }
  RemoveWorker(state.idle, worker);
}

void WorkerPool::DisconnectDriver(const std::shared_ptr<WorkerInterface> &driver) {
  auto &state = GetStateForLanguage(driver->GetLanguage());
  RAY_CHECK(RemoveWorker(state.registered_drivers, driver));
  MarkPortAsFree(driver->AssignedPort());
}

inline WorkerPool::State &WorkerPool::GetStateForLanguage(const Language &language) {
  auto state = states_by_lang_.find(language);
  RAY_CHECK(state != states_by_lang_.end())
      << "Required Language isn't supported: " << Language_Name(language);
  return state->second;
}

inline bool WorkerPool::IsIOWorkerType(const rpc::WorkerType &worker_type) const {
  return worker_type == rpc::WorkerType::SPILL_WORKER ||
         worker_type == rpc::WorkerType::RESTORE_WORKER;
}

std::vector<std::shared_ptr<WorkerInterface>> WorkerPool::GetAllRegisteredWorkers(
    bool filter_dead_workers, bool filter_io_workers) const {
  std::vector<std::shared_ptr<WorkerInterface>> workers;

  for (const auto &entry : states_by_lang_) {
    for (const auto &worker : entry.second.registered_workers) {
      if (!worker->IsRegistered()) {
        continue;
      }

      if (filter_io_workers && (IsIOWorkerType(worker->GetWorkerType()))) {
        continue;
      }

      if (filter_dead_workers && worker->IsDead()) {
        continue;
      }
      workers.push_back(worker);
    }
  }

  return workers;
}

bool WorkerPool::IsWorkerAvailableForScheduling() const {
  for (const auto &entry : states_by_lang_) {
    for (const auto &worker : entry.second.registered_workers) {
      if (!worker->IsRegistered()) {
        continue;
      }
      if (worker->IsAvailableForScheduling()) {
        return true;
      }
    }
  }
  return false;
}

std::vector<std::shared_ptr<WorkerInterface>> WorkerPool::GetAllRegisteredDrivers(
    bool filter_dead_drivers) const {
  std::vector<std::shared_ptr<WorkerInterface>> drivers;

  for (const auto &entry : states_by_lang_) {
    for (const auto &driver : entry.second.registered_drivers) {
      if (!driver->IsRegistered()) {
        continue;
      }

      if (filter_dead_drivers && driver->IsDead()) {
        continue;
      }
      drivers.push_back(driver);
    }
  }

  return drivers;
}

void WorkerPool::WarnAboutSize() {
  for (auto &entry : states_by_lang_) {
    auto &state = entry.second;
    int64_t num_workers_started_or_registered = 0;
    num_workers_started_or_registered +=
        static_cast<int64_t>(state.registered_workers.size());
    for (const auto &starting_process : state.worker_processes) {
      num_workers_started_or_registered +=
          starting_process.second.is_pending_registration ? 0 : 1;
    }
    // Don't count IO workers towards the warning message threshold.
    num_workers_started_or_registered -= RayConfig::instance().max_io_workers() * 2;
    int64_t multiple = num_workers_started_or_registered / state.multiple_for_warning;
    std::stringstream warning_message;
    if (multiple >= 4 && multiple > state.last_warning_multiple) {
      // Push an error message to the user if the worker pool tells us that it is
      // getting too big.
      state.last_warning_multiple = multiple;
      warning_message
          << "WARNING: " << num_workers_started_or_registered << " "
          << Language_Name(entry.first)
          << " worker processes have been started on node: " << node_id_
          << " with address: " << node_address_ << ". "
          << "This could be a result of using "
          << "a large number of actors, or due to tasks blocked in ray.get() calls "
          << "(see https://github.com/ray-project/ray/issues/3644 for "
          << "some discussion of workarounds).";
      std::string warning_message_str = warning_message.str();
      RAY_LOG(WARNING) << warning_message_str;

      auto error_data_ptr = gcs::CreateErrorTableData(
          "worker_pool_large", warning_message_str, get_time_());
      gcs_client_.Errors().AsyncReportJobError(error_data_ptr, nullptr);
    }
  }
}

void WorkerPool::TryStartIOWorkers(const Language &language) {
  TryStartIOWorkers(language, rpc::WorkerType::RESTORE_WORKER);
  TryStartIOWorkers(language, rpc::WorkerType::SPILL_WORKER);
}

void WorkerPool::TryPendingStartRequests(const Language &language) {
  auto &state = GetStateForLanguage(language);
  if (state.pending_start_requests.empty()) {
    return;
  }

  std::deque<std::shared_ptr<PopWorkerRequest>> pending_start_requests;
  state.pending_start_requests.swap(pending_start_requests);
  for (const auto &request : pending_start_requests) {
    StartNewWorker(request);
  }
}

void WorkerPool::TryStartIOWorkers(const Language &language,
                                   const rpc::WorkerType &worker_type) {
  if (language != Language::PYTHON) {
    return;
  }
  auto &state = GetStateForLanguage(language);
  auto &io_worker_state = GetIOWorkerStateFromWorkerType(worker_type, state);

  int available_io_workers_num =
      io_worker_state.num_starting_io_workers + io_worker_state.started_io_workers.size();
  int max_workers_to_start =
      RayConfig::instance().max_io_workers() - available_io_workers_num;
  // Compare first to prevent unsigned underflow.
  if (io_worker_state.pending_io_tasks.size() > io_worker_state.idle_io_workers.size()) {
    int expected_workers_num =
        io_worker_state.pending_io_tasks.size() - io_worker_state.idle_io_workers.size();
    if (expected_workers_num > max_workers_to_start) {
      expected_workers_num = max_workers_to_start;
    }
    for (; expected_workers_num > 0; expected_workers_num--) {
      PopWorkerStatus status;
      auto [proc, startup_token] =
          StartWorkerProcess(ray::Language::PYTHON, worker_type, JobID::Nil(), &status);
      if (!proc.IsValid()) {
        // We may hit the maximum worker start up concurrency limit. Stop.
        return;
      }
    }
  }
}

std::string WorkerPool::DebugString() const {
  std::stringstream result;
  result << "WorkerPool:";
  result << "\n- registered jobs: " << all_jobs_.size() - finished_jobs_.size();
  result << "\n- process_failed_job_config_missing: "
         << process_failed_job_config_missing_;
  result << "\n- process_failed_rate_limited: " << process_failed_rate_limited_;
  result << "\n- process_failed_pending_registration: "
         << process_failed_pending_registration_;
  result << "\n- process_failed_runtime_env_setup_failed: "
         << process_failed_runtime_env_setup_failed_;
  for (const auto &entry : states_by_lang_) {
    result << "\n- num " << Language_Name(entry.first)
           << " workers: " << entry.second.registered_workers.size();
    result << "\n- num " << Language_Name(entry.first)
           << " drivers: " << entry.second.registered_drivers.size();
    result << "\n- num " << Language_Name(entry.first)
           << " pending start requests: " << entry.second.pending_start_requests.size();
    result << "\n- num " << Language_Name(entry.first)
           << " pending registration requests: "
           << entry.second.pending_registration_requests.size();
    result << "\n- num object spill callbacks queued: "
           << entry.second.spill_io_worker_state.pending_io_tasks.size();
    result << "\n- num object restore queued: "
           << entry.second.restore_io_worker_state.pending_io_tasks.size();
    result << "\n- num util functions queued: "
           << entry.second.util_io_worker_state.pending_io_tasks.size();
  }
  result << "\n- num idle workers: " << idle_of_all_languages_.size();
  return result.str();
}

WorkerPool::IOWorkerState &WorkerPool::GetIOWorkerStateFromWorkerType(
    const rpc::WorkerType &worker_type, WorkerPool::State &state) const {
  RAY_CHECK(worker_type != rpc::WorkerType::WORKER)
      << worker_type << " type cannot be used to retrieve io_worker_state";
  switch (worker_type) {
  case rpc::WorkerType::SPILL_WORKER:
    return state.spill_io_worker_state;
  case rpc::WorkerType::RESTORE_WORKER:
    return state.restore_io_worker_state;
  default:
    RAY_LOG(FATAL) << "Unknown worker type: " << worker_type;
  }
  UNREACHABLE;
}

void WorkerPool::GetOrCreateRuntimeEnv(const std::string &serialized_runtime_env,
                                       const rpc::RuntimeEnvConfig &runtime_env_config,
                                       const JobID &job_id,
                                       const GetOrCreateRuntimeEnvCallback &callback) {
  RAY_LOG(DEBUG) << "GetOrCreateRuntimeEnv for job " << job_id << " with runtime_env "
                 << serialized_runtime_env;
  runtime_env_agent_client_->GetOrCreateRuntimeEnv(
      job_id,
      serialized_runtime_env,
      runtime_env_config,
      [job_id, serialized_runtime_env, runtime_env_config, callback](
          bool successful,
          const std::string &serialized_runtime_env_context,
          const std::string &setup_error_message) {
        if (successful) {
          callback(true, serialized_runtime_env_context, "");
        } else {
          RAY_LOG(WARNING) << "Couldn't create a runtime environment for job " << job_id
                           << ".";
          RAY_LOG(DEBUG) << "Runtime env for job " << job_id << ": "
                         << serialized_runtime_env;
          callback(/*successful=*/false,
                   /*serialized_runtime_env_context=*/"",
                   /*setup_error_message=*/setup_error_message);
        }
      });
}

void WorkerPool::DeleteRuntimeEnvIfPossible(const std::string &serialized_runtime_env) {
  RAY_LOG(DEBUG) << "DeleteRuntimeEnvIfPossible " << serialized_runtime_env;
  if (!IsRuntimeEnvEmpty(serialized_runtime_env)) {
    runtime_env_agent_client_->DeleteRuntimeEnvIfPossible(
        serialized_runtime_env, [serialized_runtime_env](bool successful) {
          if (!successful) {
            RAY_LOG(ERROR) << "Delete runtime env failed";
            RAY_LOG(DEBUG) << "Runtime env: " << serialized_runtime_env;
          }
        });
  }
}

const std::vector<std::string> &WorkerPool::LookupWorkerDynamicOptions(
    StartupToken token) const {
  for (const auto &[lang, state] : states_by_lang_) {
    auto it = state.worker_processes.find(token);
    if (it != state.worker_processes.end()) {
      return it->second.dynamic_options;
    }
  }
  static std::vector<std::string> kNoDynamicOptions;
  return kNoDynamicOptions;
}

const NodeID &WorkerPool::GetNodeID() const { return node_id_; }

}  // namespace raylet

}  // namespace ray
