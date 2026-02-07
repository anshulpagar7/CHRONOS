#include "scheduler.h"

namespace chronos {

    // -----------------------------------
    // Submit a new job
    // -----------------------------------
    void Scheduler::submit(const Job& job) {
        // Store job in lookup table
        jobs[job.job_id] = job;

        // Push job into scheduling queue
        job_queue.push(job);

        // Update state
        jobs[job.job_id].state = JobState::QUEUED;
    }

    // -----------------------------------
    // Get next job to execute
    // -----------------------------------
    bool Scheduler::get_next_job(Job& job_out) {
        if (job_queue.empty()) {
            return false;
        }

        // Fetch highest priority job
        Job next_job = job_queue.top();
        job_queue.pop();

        // Update state
        auto it = jobs.find(next_job.job_id);
        if (it != jobs.end()) {
            it->second.state = JobState::RUNNING;
            job_out = it->second;
            return true;
        }

        return false;
    }

    // -----------------------------------
    // Mark job as running
    // -----------------------------------
    void Scheduler::mark_running(uint64_t job_id) {
        auto it = jobs.find(job_id);
        if (it != jobs.end()) {
            it->second.state = JobState::RUNNING;
        }
    }

    // -----------------------------------
    // Mark job as completed
    // -----------------------------------
    void Scheduler::mark_completed(uint64_t job_id) {
        auto it = jobs.find(job_id);
        if (it != jobs.end()) {
            it->second.state = JobState::COMPLETED;
        }
    }

    // -----------------------------------
    // Mark job as failed
    // -----------------------------------
    void Scheduler::mark_failed(uint64_t job_id) {
        auto it = jobs.find(job_id);
        if (it != jobs.end()) {
            it->second.state = JobState::FAILED;
        }
    }

    // -----------------------------------
    // Check if scheduler has jobs
    // -----------------------------------
    bool Scheduler::has_jobs() const {
        return !job_queue.empty();
    }

} // namespace chronos
