#include "control/grpc_server.h"

#include <grpcpp/grpcpp.h>

#include <unordered_map>

#include "kestrel.grpc.pb.h"
#include "rpc/convert.h"

namespace kestrel {

namespace {
grpc::Status ToGrpc(const Status& s, grpc::StatusCode code = grpc::StatusCode::FAILED_PRECONDITION) {
  if (s.ok()) return grpc::Status::OK;
  if (s.message() == "no such job" || s.message() == "no such worker")
    return grpc::Status(grpc::StatusCode::NOT_FOUND, s.message());
  return grpc::Status(code, s.message());
}
}  // namespace

class ControlPlaneServer::ClientImpl final : public v1::SchedulerService::Service {
 public:
  explicit ClientImpl(ControlPlane& cp) : cp_(cp) {}

  grpc::Status SubmitJob(grpc::ServerContext*, const v1::SubmitJobRequest* req,
                         v1::SubmitJobResponse* resp) override {
    JobSpec spec = rpc::FromProto(req->spec(), 0);
    auto r = cp_.SubmitJob(std::move(spec));
    if (!r.ok()) return ToGrpc(r.status(), grpc::StatusCode::INVALID_ARGUMENT);
    resp->set_job_id(r.value());
    return grpc::Status::OK;
  }

  grpc::Status GetJob(grpc::ServerContext*, const v1::GetJobRequest* req,
                      v1::GetJobResponse* resp) override {
    std::string worker_name;
    auto r = cp_.GetJob(req->job_id(), &worker_name);
    if (!r.ok()) return ToGrpc(r.status());
    rpc::ToProto(r.value(), worker_name, resp->mutable_job());
    return grpc::Status::OK;
  }

  grpc::Status ListJobs(grpc::ServerContext*, const v1::ListJobsRequest* req,
                        v1::ListJobsResponse* resp) override {
    std::optional<JobState> filter;
    switch (req->state()) {
      case v1::JOB_STATE_PENDING: filter = JobState::PENDING; break;
      case v1::JOB_STATE_SCHEDULED: filter = JobState::SCHEDULED; break;
      case v1::JOB_STATE_RUNNING: filter = JobState::RUNNING; break;
      case v1::JOB_STATE_SUCCEEDED: filter = JobState::SUCCEEDED; break;
      case v1::JOB_STATE_FAILED: filter = JobState::FAILED; break;
      case v1::JOB_STATE_LOST: filter = JobState::LOST; break;
      case v1::JOB_STATE_CANCELLED: filter = JobState::CANCELLED; break;
      default: break;
    }
    std::unordered_map<WorkerId, std::string> names;
    for (const Worker& w : cp_.ListWorkers()) names[w.id] = w.name;
    for (const Job& j : cp_.ListJobs(filter, req->limit())) {
      const bool active = j.state == JobState::SCHEDULED || j.state == JobState::RUNNING;
      auto it = names.find(j.assigned_worker);
      rpc::ToProto(j, active && it != names.end() ? it->second : "", resp->add_jobs());
    }
    return grpc::Status::OK;
  }

  grpc::Status CancelJob(grpc::ServerContext*, const v1::CancelJobRequest* req,
                         v1::CancelJobResponse*) override {
    return ToGrpc(cp_.CancelJob(req->job_id()));
  }

  grpc::Status ListWorkers(grpc::ServerContext*, const v1::ListWorkersRequest*,
                           v1::ListWorkersResponse* resp) override {
    for (const Worker& w : cp_.ListWorkers()) rpc::ToProto(w, resp->add_workers());
    return grpc::Status::OK;
  }

 private:
  ControlPlane& cp_;
};

class ControlPlaneServer::WorkerImpl final : public v1::WorkerService::Service {
 public:
  explicit WorkerImpl(ControlPlane& cp) : cp_(cp) {}

  grpc::Status Register(grpc::ServerContext*, const v1::RegisterRequest* req,
                        v1::RegisterResponse* resp) override {
    auto running = rpc::RunningFrom(*req);
    auto completed = rpc::CompletedFrom(*req);
    auto r = cp_.Register(req->name(), rpc::FromProto(req->capacity()), running, completed);
    if (!r.ok()) return ToGrpc(r.status(), grpc::StatusCode::INVALID_ARGUMENT);
    resp->set_worker_id(r.value().worker);
    resp->set_epoch(r.value().epoch);
    for (JobId id : r.value().kill) resp->add_kill_jobs(id);
    for (JobId id : r.value().acked) resp->add_acked_jobs(id);
    return grpc::Status::OK;
  }

  grpc::Status Heartbeat(grpc::ServerContext*, const v1::HeartbeatRequest* req,
                         v1::HeartbeatResponse* resp) override {
    auto running = rpc::RunningFrom(*req);
    auto completed = rpc::CompletedFrom(*req);
    HeartbeatResult r = cp_.Heartbeat(req->worker_id(), req->epoch(), running, completed);
    resp->set_reregister(r.reregister);
    for (const AssignmentOrder& a : r.assignments) {
      v1::Assignment* out = resp->add_assignments();
      out->set_job_id(a.job);
      out->set_generation(a.generation);
      rpc::ToProto(a.spec, out->mutable_spec());
    }
    for (JobId id : r.kill) resp->add_kill_jobs(id);
    for (JobId id : r.acked) resp->add_acked_jobs(id);
    return grpc::Status::OK;
  }

 private:
  ControlPlane& cp_;
};

ControlPlaneServer::ControlPlaneServer(ControlPlane& cp)
    : cp_(cp),
      client_service_(std::make_unique<ClientImpl>(cp)),
      worker_service_(std::make_unique<WorkerImpl>(cp)) {}

ControlPlaneServer::~ControlPlaneServer() {
  Shutdown();
  server_.reset();
}

Result<int> ControlPlaneServer::Start(const std::string& address) {
  grpc::ServerBuilder builder;
  // Workers send keepalive pings every 3 s (see worker_agent.cpp). By default a server treats
  // pings more often than every 5 minutes as abuse and sends GOAWAY(too_many_pings).
  builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 1000);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
  int port = 0;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(client_service_.get());
  builder.RegisterService(worker_service_.get());
  server_ = builder.BuildAndStart();
  if (!server_ || port == 0) return Status::Error("failed to listen on " + address);
  return port;
}

void ControlPlaneServer::Shutdown() {
  // Safe to call from a signal-handling thread while another thread is in Wait(): the Server
  // object stays alive until the destructor.
  if (server_) server_->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
}

void ControlPlaneServer::Wait() {
  if (server_) server_->Wait();
}

}  // namespace kestrel
