// kestrel_sim: run the placement-policy comparison in the virtual-time simulator.
//
//   kestrel_sim --policy all --seeds 20 --workers 10 --jobs 2000 --load 0.85 --csv out.csv
//
// Every run prints its full configuration first, so a result can always be regenerated.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/placement_policy.h"
#include "sim/simulator.h"
#include "sim/workload.h"

using namespace kestrel;
using namespace kestrel::sim;

namespace {

struct Args {
  std::string policy = "all";
  uint64_t first_seed = 1;
  int num_seeds = 10;
  int workers = 10;
  int jobs = 2000;
  double load = 0.85;
  std::string csv;
  bool check = false;
};

void Usage() {
  std::fprintf(stderr,
               "usage: kestrel_sim [--policy rr|ff|bf|all] [--first-seed N] [--seeds COUNT]\n"
               "                   [--workers N] [--jobs N] [--load F] [--csv FILE] [--check]\n");
  std::exit(2);
}

Args Parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&]() -> const char* {
      if (i + 1 >= argc) Usage();
      return argv[++i];
    };
    if (k == "--policy") a.policy = val();
    else if (k == "--first-seed") a.first_seed = std::strtoull(val(), nullptr, 10);
    else if (k == "--seeds") a.num_seeds = std::atoi(val());
    else if (k == "--workers") a.workers = std::atoi(val());
    else if (k == "--jobs") a.jobs = std::atoi(val());
    else if (k == "--load") a.load = std::atof(val());
    else if (k == "--csv") a.csv = val();
    else if (k == "--check") a.check = true;
    else Usage();
  }
  if (a.num_seeds < 1 || a.workers < 1 || a.jobs < 1 || a.load <= 0) Usage();
  return a;
}

const char* kCsvHeader =
    "policy,seed,workers,jobs,load,makespan_s,cpu_util_makespan,mem_util_makespan,cpu_util_busy,"
    "mem_util_busy,busy_fraction,frag_fraction_makespan,frag_fraction_busy,wait_p50_s,wait_p99_s,"
    "wait_mean_s,max_queue_depth,jobs_succeeded\n";

double Sec(TimeUs t) { return static_cast<double>(t) / 1e6; }

}  // namespace

int main(int argc, char** argv) {
  Args a = Parse(argc, argv);
  std::vector<std::string> policies;
  if (a.policy == "all") policies = {"round-robin", "first-fit", "best-fit"};
  else if (auto p = MakePolicy(a.policy)) policies.push_back(p->Name());
  else Usage();

  WorkloadConfig base;
  base.num_jobs = a.jobs;
  base.target_load = a.load;
  base.fleet = MakeFleet(a.workers);

  std::printf("# kestrel_sim policies=%s first_seed=%llu seeds=%d workers=%d jobs=%d load=%.3f\n",
              a.policy.c_str(), static_cast<unsigned long long>(a.first_seed), a.num_seeds,
              a.workers, a.jobs, a.load);
  std::printf("# fleet cycles 2c/8GiB, 4c/16GiB, 8c/32GiB; mean inter-arrival %.1f ms\n",
              static_cast<double>(MeanInterarrival(base)) / 1000.0);
  for (const JobClass& c : base.classes)
    std::printf("# class %-9s cpu=%lldm mem=%lldMiB dur=[%.0f,%.0f]s weight=%u\n", c.name.c_str(),
                static_cast<long long>(c.request.cpu_millis),
                static_cast<long long>(c.request.mem_mib), Sec(c.min_duration),
                Sec(c.max_duration), c.weight);

  std::ofstream csv;
  if (!a.csv.empty()) {
    csv.open(a.csv);
    csv << kCsvHeader;
  }

  std::map<std::string, std::vector<SimResult>> by_policy;
  for (int s = 0; s < a.num_seeds; ++s) {
    WorkloadConfig cfg = base;
    cfg.seed = a.first_seed + static_cast<uint64_t>(s);
    std::vector<SimJob> jobs = GenerateWorkload(cfg);  // identical stream for every policy
    for (const std::string& p : policies) {
      SimOptions opts;
      opts.check_invariants = a.check;
      SimResult r = RunSimulation(p, cfg.seed, cfg.fleet, jobs, opts);
      by_policy[p].push_back(r);
      if (csv.is_open()) {
        char buf[512];
        std::snprintf(buf, sizeof buf,
                      "%s,%llu,%d,%d,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,"
                      "%zu,%zu\n",
                      r.policy.c_str(), static_cast<unsigned long long>(r.seed), a.workers, a.jobs,
                      a.load, Sec(r.makespan_us), r.cpu_util_makespan, r.mem_util_makespan,
                      r.cpu_util_busy, r.mem_util_busy, r.busy_fraction, r.frag_fraction_makespan,
                      r.frag_fraction_busy, Sec(r.wait_p50_us), Sec(r.wait_p99_us),
                      r.wait_mean_us / 1e6, r.max_queue_depth, r.jobs_succeeded);
        csv << buf;
      }
    }
  }

  std::printf("\n%-12s %9s %9s %9s %9s %9s %9s %9s %9s %9s\n", "policy", "makespan", "cpu_mk",
              "mem_mk", "cpu_busy", "mem_busy", "frag_mk", "frag_busy", "wait_p50", "wait_p99");
  for (const std::string& p : policies) {
    const auto& v = by_policy[p];
    double mk = 0, cm = 0, mm = 0, cb = 0, mb = 0, fm = 0, fb = 0, w50 = 0, w99 = 0;
    for (const SimResult& r : v) {
      mk += Sec(r.makespan_us); cm += r.cpu_util_makespan; mm += r.mem_util_makespan;
      cb += r.cpu_util_busy; mb += r.mem_util_busy; fm += r.frag_fraction_makespan;
      fb += r.frag_fraction_busy; w50 += Sec(r.wait_p50_us); w99 += Sec(r.wait_p99_us);
    }
    const double n = static_cast<double>(v.size());
    std::printf("%-12s %8.1fs %9.4f %9.4f %9.4f %9.4f %9.4f %9.4f %8.2fs %8.2fs\n", p.c_str(),
                mk / n, cm / n, mm / n, cb / n, mb / n, fm / n, fb / n, w50 / n, w99 / n);
  }
  std::printf("(means over %d seeds; per-seed rows in the CSV)\n", a.num_seeds);
  return 0;
}
