#include <iostream>
#include <chrono>

#include "scheduler.h"

using namespace chronos;

int main() {
    Scheduler scheduler;

    auto now = std::chrono::steady_clock::now();

    // Create some test jobs
    Job job1(1, 10, now, now + std::chrono::seconds(10), 3, 1, 256);
    Job job2(2, 5,  now, now + std::chrono::seconds(20), 3, 1, 256);
    Job job3(3, 20, now, now + std::chrono::seconds(5),  3, 1, 256);

    // Submit jobs
    scheduler.submit(job1);
    scheduler.submit(job2);
    scheduler.submit(job3);

    // Fetch jobs in scheduling order
    Job next;
    while (scheduler.get_next_job(next)) {
        std::cout << "Executing Job ID: " << next.job_id
                  << " | Priority: " << next.priority << std::endl;

        // Simulate success
        scheduler.mark_completed(next.job_id);
    }

    return 0;
}
