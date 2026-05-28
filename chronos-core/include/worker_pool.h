#ifndef CHRONOS_WORKER_POOL_H
#define CHRONOS_WORKER_POOL_H

#include <vector>
#include <memory>
#include <cstdint>

#include "worker.h"

namespace chronos {

    class WorkerPool {
    public:
        WorkerPool(uint64_t worker_count,
                   Scheduler& scheduler);

        void start();

        void stop();

    private:
        uint64_t total_workers;

        Scheduler& scheduler_ref;

        std::vector<std::unique_ptr<Worker>> workers;
    };

} // namespace chronos

#endif // CHRONOS_WORKER_POOL_H