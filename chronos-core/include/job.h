#ifndef CHRONOS_JOB_H
#define CHRONOS_JOB_H

#include <cstdint>
#include <chrono>
#include <string>

namespace chronos {

    // -------------------------------
    // Job lifecycle states
    // -------------------------------
    enum class JobState {
        SUBMITTED,
        QUEUED,
        RUNNING,
        FAILED,
        RETRYING,
        COMPLETED
    };

    // -------------------------------
    // Job definition
    // -------------------------------
    struct Job {
        // Identity
        uint64_t job_id;

        // Scheduling constraints
        int priority;
        std::chrono::steady_clock::time_point submit_time;
        std::chrono::steady_clock::time_point deadline;

        // Reliability
        int max_retries;
        int current_retry;

        // Resource requirements
        int cpu_units;
        int memory_mb;

        // State
        JobState state;

        // Constructor
        Job(uint64_t id,
            int prio,
            std::chrono::steady_clock::time_point submit,
            std::chrono::steady_clock::time_point dl,
            int retries,
            int cpu,
            int memory)
            : job_id(id),
              priority(prio),
              submit_time(submit),
              deadline(dl),
              max_retries(retries),
              current_retry(0),
              cpu_units(cpu),
              memory_mb(memory),
              state(JobState::SUBMITTED) {}
    };

} // namespace chronos

#endif // CHRONOS_JOB_H
