// Job, Worker and the journal Record: the data model from DESIGN.md section 3.
#pragma once

#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/types.h"

namespace kestrel {

struct JobSpec {
  std::string command;
  std::vector<std::string> args;
  Resources request;
  int max_attempts = 3;
};

struct Job {
  JobId id = 0;
  JobSpec spec;
  JobState state = JobState::PENDING;
  int attempts = 0;                // number of times the job has been assigned
  WorkerId assigned_worker = 0;    // valid while SCHEDULED/RUNNING
  uint64_t generation = 0;         // assignment_generation; bumped on every assignment
  TimeUs submitted = kUnsetTime;
  TimeUs first_scheduled = kUnsetTime;  // first assignment (queue wait time is measured to here)
  TimeUs scheduled = kUnsetTime;        // latest assignment
  TimeUs started = kUnsetTime;
  TimeUs finished = kUnsetTime;
  int exit_code = 0;
};

struct Worker {
  WorkerId id = 0;
  std::string name;  // stable identity chosen by the agent (pod name); id is derived from it
  Epoch epoch = 0;
  Resources capacity;
  Resources allocated;  // sum of requests of SCHEDULED/RUNNING jobs
  TimeUs last_heartbeat = kUnsetTime;
  WorkerStatus status = WorkerStatus::ALIVE;
  std::set<JobId> jobs;  // SCHEDULED/RUNNING jobs assigned here

  Resources Free() const { return capacity - allocated; }
};

// Every state mutation is expressed as a Record. A mutator validates, builds Records,
// hands them to the Journal (write-ahead), and only then applies them. Replay after a
// restart feeds the journaled Records through the very same Apply().
enum class RecordType : uint8_t {
  SUBMIT = 1,
  ASSIGN = 2,
  FINISH = 3,
  LOST = 4,
  CANCEL = 5,
  REGISTER = 6,
};

struct Record {
  RecordType type = RecordType::SUBMIT;
  TimeUs time = 0;
  JobId job = 0;
  WorkerId worker = 0;
  uint64_t generation = 0;
  Epoch epoch = 0;
  int32_t exit_code = 0;
  JobSpec spec;       // SUBMIT
  std::string name;   // REGISTER
  Resources capacity; // REGISTER

  bool operator==(const Record& o) const {
    return type == o.type && time == o.time && job == o.job && worker == o.worker &&
           generation == o.generation && epoch == o.epoch && exit_code == o.exit_code &&
           spec.command == o.spec.command && spec.args == o.spec.args &&
           spec.request == o.spec.request && spec.max_attempts == o.spec.max_attempts &&
           name == o.name && capacity == o.capacity;
  }
};

// Durable append-only sink. Append must not return ok until the records are durable
// (as durable as the implementation promises). All records in one call are one batch.
class Journal {
 public:
  virtual ~Journal() = default;
  virtual Status Append(std::span<const Record> records) = 0;
};

}  // namespace kestrel
