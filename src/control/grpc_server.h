// gRPC front end for ControlPlane: SchedulerService (clients) and WorkerService (agents).
#pragma once

#include <memory>
#include <string>

#include "control/control_plane.h"

namespace grpc {
class Server;
}

namespace kestrel {

class ControlPlaneServer {
 public:
  explicit ControlPlaneServer(ControlPlane& cp);
  ~ControlPlaneServer();

  // Listens on `address` (e.g. "0.0.0.0:7000" or "127.0.0.1:0"). Returns the bound port.
  Result<int> Start(const std::string& address);
  void Shutdown();
  void Wait();

 private:
  class ClientImpl;
  class WorkerImpl;
  ControlPlane& cp_;
  std::unique_ptr<ClientImpl> client_service_;
  std::unique_ptr<WorkerImpl> worker_service_;
  std::unique_ptr<grpc::Server> server_;
};

}  // namespace kestrel
