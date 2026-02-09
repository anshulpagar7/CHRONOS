#include "scheduler.h"

namespace chronos {

    Scheduler::Scheduler()
        : base_backoff(std::chrono::milliseconds(1000)) {}

    void Scheduler::submit(const Job& job) {
        Job new_job = job;
        new_job.state = JobState::QUEUED;

        jobs[job.job_id] = new_job;
        ready_queue.push(new_job);
    }

    bool Scheduler::get_next_job(Job& job_out) {
        process_retries();

        if (ready_queue.empty()) {
            return false;
        }

        Job job = ready_queue.top();
        ready_queue.pop();

        auto it = jobs.find(job.job_id);
        if (it == jobs.end()) {
            return false;
        }

        it->second.state = JobState::RUNNING;
        job_out = it->second;
        return true;
    }

    void Scheduler::mark_completed(uint64_t job_id) {
        auto it = jobs.find(job_id);
        if (it != jobs.end()) {
            it->second.state = JobState::COMPLETED;
        }
    }

    void Scheduler::mark_failed(uint64_t job_id) {
        auto it = jobs.find(job_id);
        if (it == jobs.end()) return;

        Job& job = it->second;
        job.current_retry++;

        if (job.current_retry <= job.max_retries) {
            job.state = JobState::RETRYING;

            auto now = std::chrono::steady_clock::now();
            auto delay = base_backoff * (1 << (job.current_retry - 1));

            job.next_retry_time = now + delay;
            retry_queue.push(job);
        } else {
            job.state = JobState::FAILED;
        }
    }

    void Scheduler::process_retries() {
        auto now = std::chrono::steady_clock::now();

        while (!retry_queue.empty()) {
            Job job = retry_queue.top();

            if (job.next_retry_time > now) break;

            retry_queue.pop();

            auto it = jobs.find(job.job_id);
            if (it == jobs.end()) continue;

            it->second.state = JobState::QUEUED;
            ready_queue.push(it->second);
        }
    }

    bool Scheduler::has_jobs() const {
        return !ready_queue.empty() || !retry_queue.empty();
    }

} // namespace chronos
