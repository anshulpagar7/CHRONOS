#include "worker_pool.h"

namespace chronos {

    WorkerPool::WorkerPool(uint64_t worker_count,
                           Scheduler& scheduler)
        : total_workers(worker_count),
          scheduler_ref(scheduler) {

        for (uint64_t i = 0; i < total_workers; ++i) {

            workers.push_back(
                std::make_unique<Worker>(
                    i + 1,
                    scheduler_ref));
        }
    }

    // -----------------------------------
    // Start all workers
    // -----------------------------------
    void WorkerPool::start() {

        for (auto& worker : workers) {
            worker->start();
        }
    }

    // -----------------------------------
    // Stop all workers
    // -----------------------------------
    void WorkerPool::stop() {

        for (auto& worker : workers) {
            worker->stop();
        }
    }

} // namespace chronos