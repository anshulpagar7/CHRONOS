#ifndef CHRONOS_SCHEDULER_H
#define CHRONOS_SCHEDULER_H

#include <queue>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <chrono>

#include "job.h"

namespace chronos {

    // Comparator for ready jobs (priority-based for now)
    struct JobComparator {
        bool operator()(const Job& a, const Job& b) const {
            return a.priority < b.priority; // higher priority first
        }
    };

    // Comparator for retry queue (earliest retry time first)
    struct RetryComparator {
        bool operator()(const Job& a, const Job& b) const {
            return a.next_retry_time > b.next_retry_time;
        }
    };

    class Scheduler {
    public:
        Scheduler();

        void submit(const Job& job);

        bool get_next_job(Job& job_out);

        void mark_completed(uint64_t job_id);
        void mark_failed(uint64_t job_id);

        void process_retries();

        bool has_jobs() const;

    private:
        std::priority_queue<Job, std::vector<Job>, JobComparator> ready_queue;
        std::priority_queue<Job, std::vector<Job>, RetryComparator> retry_queue;

        std::unordered_map<uint64_t, Job> jobs;

        std::chrono::milliseconds base_backoff;
    };

} // namespace chronos

#endif // CHRONOS_SCHEDULER_H
