#ifndef CHRONOS_SCHEDULER_H
#define CHRONOS_SCHEDULER_H

#include <queue>
#include <unordered_map>
#include <cstdint>

#include "job.h"

namespace chronos {

    // -----------------------------------
    // Comparator for scheduling decisions
    // -----------------------------------
    struct JobComparator {
        bool operator()(const Job& a, const Job& b) const {
            // Higher score should come first
            // Placeholder logic: higher priority wins
            return a.priority < b.priority;
        }
    };

    // -----------------------------------
    // Scheduler interface
    // -----------------------------------
    class Scheduler {
    public:
        Scheduler() = default;

        // Submit a new job
        void submit(const Job& job);

        // Fetch next job to execute
        bool get_next_job(Job& job_out);

        // Job state transitions
        void mark_running(uint64_t job_id);
        void mark_completed(uint64_t job_id);
        void mark_failed(uint64_t job_id);

        // Scheduler state
        bool has_jobs() const;

    private:
        // Priority queue of jobs
        std::priority_queue<Job, std::vector<Job>, JobComparator> job_queue;

        // Job lookup table
        std::unordered_map<uint64_t, Job> jobs;
    };

} // namespace chronos

#endif // CHRONOS_SCHEDULER_H
