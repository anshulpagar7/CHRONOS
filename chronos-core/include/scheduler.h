#ifndef CHRONOS_SCHEDULER_H
#define CHRONOS_SCHEDULER_H

#include <queue>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <chrono>

#include "job.h"

namespace chronos {

    // -----------------------------------
    // Smart scheduling comparator
    // -----------------------------------
    struct JobComparator {
        bool operator()(const Job& a, const Job& b) const {
            auto now = std::chrono::steady_clock::now();

            // ---------- Priority ----------
            double score_a = a.priority;
            double score_b = b.priority;

            // ---------- Deadline urgency ----------
            auto da = std::chrono::duration_cast<std::chrono::milliseconds>(
                a.deadline - now).count();
            auto db = std::chrono::duration_cast<std::chrono::milliseconds>(
                b.deadline - now).count();

            if (da > 0) score_a += 10000.0 / da;
            if (db > 0) score_b += 10000.0 / db;

            // ---------- Fairness / aging ----------
            auto wa = std::chrono::duration_cast<std::chrono::seconds>(
                now - a.submit_time).count();
            auto wb = std::chrono::duration_cast<std::chrono::seconds>(
                now - b.submit_time).count();

            score_a += wa * 0.5;
            score_b += wb * 0.5;

            return score_a < score_b; // max-heap behavior
        }
    };

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
