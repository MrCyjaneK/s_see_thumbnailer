#include "jobs.h"

#include <map>
#include <mutex>
#include <thread>

namespace thumbnailer::jobs {

namespace {

enum class Status { Running, Done, Error };

struct Job {
  Status status = Status::Running;
  int progress = 0;
  std::string message = "starting";
  std::string error;
};

std::mutex mu;
std::map<JobId, Job> jobs;
JobId next_id = 1;

void set_progress(JobId id, int p, const std::string& msg) {
  std::lock_guard<std::mutex> lock(mu);
  auto& j = jobs[id];
  j.progress = p;
  if (!msg.empty()) j.message = msg;
}

void set_done(JobId id) {
  std::lock_guard<std::mutex> lock(mu);
  auto& j = jobs[id];
  j.status = Status::Done;
  j.progress = 100;
  j.message = "done";
}

void set_error(JobId id, const std::string& err) {
  std::lock_guard<std::mutex> lock(mu);
  auto& j = jobs[id];
  j.status = Status::Error;
  j.error = err;
  j.message = err;
  j.progress = 100;
}

}  // namespace

void run_analyze(JobId id, std::string input, std::string output);

void start_analyze(JobId id, std::string input, std::string output) {
  {
    std::lock_guard<std::mutex> lock(mu);
    jobs[id] = Job{};
  }
  std::thread([id, input = std::move(input), output = std::move(output)]() mutable {
    run_analyze(id, std::move(input), std::move(output));
  }).detach();
}

JobId new_job_id() {
  std::lock_guard<std::mutex> lock(mu);
  return next_id++;
}

int progress(JobId id, int scale) {
  std::lock_guard<std::mutex> lock(mu);
  auto it = jobs.find(id);
  if (it == jobs.end()) return 0;
  const auto& j = it->second;
  if (j.status == Status::Done || j.status == Status::Error) return scale;
  int p = j.progress;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return (p * scale) / 100;
}

std::string progress_string(JobId id) {
  std::lock_guard<std::mutex> lock(mu);
  auto it = jobs.find(id);
  if (it == jobs.end()) return "unknown job";
  if (!it->second.error.empty()) return it->second.error;
  return it->second.message;
}

void update_progress(JobId id, int p, const std::string& msg) { set_progress(id, p, msg); }
void finish_job(JobId id) { set_done(id); }
void fail_job(JobId id, const std::string& err) { set_error(id, err); }

}  // namespace thumbnailer::jobs
