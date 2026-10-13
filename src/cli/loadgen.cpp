// kestrel_loadgen: replays the simulator's seeded synthetic workload against a REAL cluster.
//
//   kestrel_loadgen --control host:7000 --seed 1 --jobs 300 --workers 9 --load 0.85 \
//                   --time-scale 10 --csv jobs.csv --label best-fit
//
// The workload (classes, durations, arrival gaps) is exactly what `kestrel_sim` generates for the
// same seed/workers/load; `--time-scale F` divides every duration and gap by F so a run takes
// seconds, not hours. Job bodies are `sleep <seconds>`. The tool submits at the scaled arrival
// times, samples cluster state every 100 ms, waits for every job and prints a summary line using
// the same metric names as the simulator.
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <fstream>
#include <map>
#include <thread>

#include "common/flags.h"
#include "kestrel.grpc.pb.h"
#include "sim/simulator.h"
#include "sim/workload.h"

using namespace kestrel;
using Clock = std::chrono::steady_clock;

namespace {

struct Sample {
  double dt = 0;
  double cpu_alloc = 0, mem_alloc = 0;  // fractions of ALIVE capacity
  bool queue_nonempty = false;
  bool head_frag_blocked = false;
};

}  // namespace

int main(int argc, char** argv) {
  Flags f(argc, argv);
  const std::string addr = f.Str("control", "127.0.0.1:7000");
  const uint64_t seed = static_cast<uint64_t>(f.Int("seed", 1));
  const int njobs = static_cast<int>(f.Int("jobs", 200));
  const int nworkers = static_cast<int>(f.Int("workers", 9));
  const double load = f.Double("load", 0.85);
  const double scale = f.Double("time-scale", 10.0);
  const std::string label = f.Str("label", "run");
  const std::string csv = f.Str("csv");
  const int timeout_s = static_cast<int>(f.Int("timeout-s", 600));
  const std::string id_prefix = f.Str("request-prefix", label + "-s" + std::to_string(seed) + "-");

  sim::WorkloadConfig wc;
  wc.seed = seed;
  wc.num_jobs = njobs;
  wc.target_load = load;
  wc.fleet = sim::MakeFleet(nworkers);
  std::vector<sim::SimJob> jobs = sim::GenerateWorkload(wc);

  auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
  auto stub = v1::SchedulerService::NewStub(channel);

  std::printf("# kestrel_loadgen label=%s seed=%llu jobs=%d workers=%d load=%.3f time_scale=%.1f "
              "control=%s\n",
              label.c_str(), static_cast<unsigned long long>(seed), njobs, nworkers, load, scale,
              addr.c_str());

  // ---- sampler thread: cluster utilisation and queue state every 100 ms ----
  std::vector<Sample> samples;
  std::atomic<bool> stop_sampling{false};
  std::thread sampler([&] {
    auto last = Clock::now();
    while (!stop_sampling.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      auto now = Clock::now();
      Sample s;
      s.dt = std::chrono::duration<double>(now - last).count();
      last = now;
      v1::ListWorkersResponse wr;
      grpc::ClientContext c1;
      if (!stub->ListWorkers(&c1, v1::ListWorkersRequest(), &wr).ok()) continue;
      double cap_c = 0, cap_m = 0, al_c = 0, al_m = 0, free_c = 0, free_m = 0;
      for (const auto& w : wr.workers()) {
        if (w.status() != v1::WORKER_STATUS_ALIVE) continue;
        cap_c += static_cast<double>(w.capacity().cpu_millis());
        cap_m += static_cast<double>(w.capacity().mem_mib());
        al_c += static_cast<double>(w.allocated().cpu_millis());
        al_m += static_cast<double>(w.allocated().mem_mib());
        free_c += static_cast<double>(w.capacity().cpu_millis() - w.allocated().cpu_millis());
        free_m += static_cast<double>(w.capacity().mem_mib() - w.allocated().mem_mib());
      }
      if (cap_c <= 0 || cap_m <= 0) continue;
      s.cpu_alloc = al_c / cap_c;
      s.mem_alloc = al_m / cap_m;
      v1::ListJobsRequest lr;
      lr.set_state(v1::JOB_STATE_PENDING);
      lr.set_limit(1);  // lowest id first == the FIFO head
      v1::ListJobsResponse jr;
      grpc::ClientContext c2;
      if (stub->ListJobs(&c2, lr, &jr).ok() && jr.jobs_size() > 0) {
        s.queue_nonempty = true;
        const auto& head = jr.jobs(0).spec().request();
        s.head_frag_blocked = static_cast<double>(head.cpu_millis()) <= free_c &&
                              static_cast<double>(head.mem_mib()) <= free_m;
      }
      samples.push_back(s);
    }
  });

  // ---- submit at scaled arrival times ----
  const auto t_start = Clock::now();
  std::vector<uint64_t> ids(jobs.size(), 0);
  int submit_errors = 0;
  for (size_t i = 0; i < jobs.size(); ++i) {
    auto due = t_start + std::chrono::microseconds(static_cast<int64_t>(
                             static_cast<double>(jobs[i].arrival) / scale));
    std::this_thread::sleep_until(due);
    v1::SubmitJobRequest req;
    auto* spec = req.mutable_spec();
    spec->set_command("sleep");
    char dur[32];
    std::snprintf(dur, sizeof dur, "%.4f", static_cast<double>(jobs[i].duration) / 1e6 / scale);
    spec->add_args(dur);
    spec->mutable_request()->set_cpu_millis(jobs[i].spec.request.cpu_millis);
    spec->mutable_request()->set_mem_mib(jobs[i].spec.request.mem_mib);
    spec->set_max_attempts(5);
    spec->set_request_id(id_prefix + std::to_string(i));  // makes a retried submit idempotent
    v1::SubmitJobResponse resp;
    grpc::Status st;
    for (int attempt = 0; attempt < 5; ++attempt) {
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
      st = stub->SubmitJob(&ctx, req, &resp);
      if (st.ok()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (st.ok()) ids[i] = resp.job_id();
    else ++submit_errors;
  }
  const double submit_s = std::chrono::duration<double>(Clock::now() - t_start).count();

  // ---- wait for completion ----
  std::map<uint64_t, v1::JobInfo> final_jobs;
  const auto deadline = Clock::now() + std::chrono::seconds(timeout_s);
  while (Clock::now() < deadline) {
    v1::ListJobsResponse all;
    grpc::ClientContext ctx;
    if (!stub->ListJobs(&ctx, v1::ListJobsRequest(), &all).ok()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    size_t terminal = 0;
    std::map<uint64_t, v1::JobInfo> snap;
    for (const auto& j : all.jobs()) {
      snap[j.id()] = j;
      const auto s = j.state();
      terminal += (s == v1::JOB_STATE_SUCCEEDED || s == v1::JOB_STATE_FAILED ||
                   s == v1::JOB_STATE_CANCELLED);
    }
    final_jobs = std::move(snap);
    size_t ours_done = 0;
    for (uint64_t id : ids) {
      if (id == 0) continue;
      auto it = final_jobs.find(id);
      if (it == final_jobs.end()) continue;
      const auto s = it->second.state();
      ours_done += (s == v1::JOB_STATE_SUCCEEDED || s == v1::JOB_STATE_FAILED ||
                    s == v1::JOB_STATE_CANCELLED);
    }
    if (ours_done + static_cast<size_t>(submit_errors) >= ids.size()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  stop_sampling = true;
  sampler.join();

  // ---- summarise (control-plane timestamps are all from one clock) ----
  size_t succeeded = 0, failed = 0, unfinished = 0, retried = 0;
  std::vector<TimeUs> waits;
  int64_t first_submit = INT64_MAX, last_finish = 0;
  std::ofstream out;
  if (!csv.empty()) {
    out.open(csv);
    out << "label,seed,index,job_id,class,state,attempts,submitted_us,first_scheduled_us,"
           "started_us,finished_us,cpu_millis,mem_mib\n";
  }
  for (size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == 0) continue;
    auto it = final_jobs.find(ids[i]);
    if (it == final_jobs.end()) {
      ++unfinished;
      continue;
    }
    const auto& j = it->second;
    if (j.state() == v1::JOB_STATE_SUCCEEDED) ++succeeded;
    else if (j.state() == v1::JOB_STATE_FAILED || j.state() == v1::JOB_STATE_CANCELLED) ++failed;
    else ++unfinished;
    if (j.attempts() > 1) ++retried;
    if (j.first_scheduled_us() >= 0 && j.submitted_us() >= 0)
      waits.push_back(j.first_scheduled_us() - j.submitted_us());
    first_submit = std::min<int64_t>(first_submit, j.submitted_us());
    last_finish = std::max<int64_t>(last_finish, j.finished_us());
    if (out.is_open())
      out << label << "," << seed << "," << i << "," << j.id() << "," << jobs[i].job_class << ","
          << j.state() << "," << j.attempts() << "," << j.submitted_us() << ","
          << j.first_scheduled_us() << "," << j.started_us() << "," << j.finished_us() << ","
          << jobs[i].spec.request.cpu_millis << "," << jobs[i].spec.request.mem_mib << "\n";
  }
  double span = 0, cpu_area = 0, mem_area = 0, busy = 0, cpu_busy = 0, mem_busy = 0, frag = 0;
  for (const Sample& s : samples) {
    span += s.dt;
    cpu_area += s.cpu_alloc * s.dt;
    mem_area += s.mem_alloc * s.dt;
    if (s.queue_nonempty) {
      busy += s.dt;
      cpu_busy += s.cpu_alloc * s.dt;
      mem_busy += s.mem_alloc * s.dt;
      if (s.head_frag_blocked) frag += s.dt;
    }
  }
  const double makespan_s = last_finish > first_submit ? static_cast<double>(last_finish - first_submit) / 1e6 : 0;
  auto sec = [](TimeUs t) { return static_cast<double>(t) / 1e6; };
  std::printf(
      "RESULT label=%s seed=%llu jobs=%zu succeeded=%zu failed=%zu unfinished=%zu retried=%zu "
      "submit_errors=%d makespan_s=%.3f submit_phase_s=%.3f cpu_util_makespan=%.4f "
      "mem_util_makespan=%.4f cpu_util_busy=%.4f mem_util_busy=%.4f busy_fraction=%.4f "
      "frag_fraction_makespan=%.4f frag_fraction_busy=%.4f queue_wait_p50_s=%.3f "
      "queue_wait_p99_s=%.3f samples=%zu\n",
      label.c_str(), static_cast<unsigned long long>(seed), ids.size(), succeeded, failed,
      unfinished, retried, submit_errors, makespan_s, submit_s, span > 0 ? cpu_area / span : 0,
      span > 0 ? mem_area / span : 0, busy > 0 ? cpu_busy / busy : 0, busy > 0 ? mem_busy / busy : 0,
      span > 0 ? busy / span : 0, span > 0 ? frag / span : 0, busy > 0 ? frag / busy : 0,
      sec(sim::Percentile(waits, 0.50)), sec(sim::Percentile(waits, 0.99)), samples.size());
  return unfinished == 0 && submit_errors == 0 ? 0 : 1;
}
