#include "worker.h"

#include <iostream>
#include <chrono>
#include <thread>

namespace chronos {

    Worker::Worker(uint64_t id, Scheduler& scheduler)
        : worker_id(id),
          scheduler_ref(scheduler),
          running(false) {}

    // -----------------------------------
    // Start worker thread
    // -----------------------------------
    void Worker::start() {
        running = true;

        worker_thread = std::thread(&Worker::run, this);
    }

    // -----------------------------------
    // Stop worker thread
    // -----------------------------------
    void Worker::stop() {
        running = false;

        if (worker_thread.joinable()) {
            worker_thread.join();
        }
    }

    // -----------------------------------
    // Worker execution loop
    // -----------------------------------
    void Worker::run() {

        while (running) {

            Job job(0, 0,
                    std::chrono::steady_clock::now(),
                    std::chrono::steady_clock::now(),
                    0, 0, 0);

            bool found_job =
                scheduler_ref.get_next_job(job);

            if (found_job) {

                std::cout
                    << "[Worker "
                    << worker_id
                    << "] Executing Job "
                    << job.job_id
                    << std::endl;

                // Simulated execution
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(500));

                // Simulated success/failure
                bool success =
                    (job.job_id % 3 != 0);

                if (success) {

                    scheduler_ref.mark_completed(
                        job.job_id);

                    std::cout
                        << "[Worker "
                        << worker_id
                        << "] Completed Job "
                        << job.job_id
                        << std::endl;

                } else {

                    scheduler_ref.mark_failed(
                        job.job_id);

                    std::cout
                        << "[Worker "
                        << worker_id
                        << "] Failed Job "
                        << job.job_id
                        << std::endl;
                }

            } else {

                // No job available
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
            }
        }
    }

} // namespace chronos