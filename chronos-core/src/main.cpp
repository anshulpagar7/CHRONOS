#include <iostream>
#include <chrono>
#include <thread>

#include "scheduler.h"
#include "worker_pool.h"

using namespace chronos;

int main() {

    // -----------------------------------
    // Create scheduler
    // -----------------------------------
    Scheduler scheduler;

    // -----------------------------------
    // Create worker pool
    // -----------------------------------
    WorkerPool pool(3, scheduler);

    // Start workers
    pool.start();

    auto now = std::chrono::steady_clock::now();

    // -----------------------------------
    // Submit jobs
    // -----------------------------------
    for (uint64_t i = 1; i <= 15; ++i) {

        int priority = (i % 5) + 1;

        Job job(
            i,
            priority,
            now,
            now + std::chrono::seconds(20),
            3,      // max retries
            1,      // cpu units
            256     // memory
        );

        scheduler.submit(job);

        std::cout
            << "[Main] Submitted Job "
            << i
            << " with priority "
            << priority
            << std::endl;
    }

    // -----------------------------------
    // Let workers process jobs
    // -----------------------------------
    std::this_thread::sleep_for(
        std::chrono::seconds(15));

    // -----------------------------------
    // Shutdown worker pool
    // -----------------------------------
    pool.stop();

    std::cout
        << "\nCHRONOS shutdown complete."
        << std::endl;

    return 0;
}