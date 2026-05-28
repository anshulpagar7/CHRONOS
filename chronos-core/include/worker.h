#ifndef CHRONOS_WORKER_H
#define CHRONOS_WORKER_H

#include <thread>
#include <atomic>
#include <cstdint>

#include "scheduler.h"

namespace chronos {

    class Worker {
    public:
        Worker(uint64_t id, Scheduler& scheduler);

        void start();
        void stop();

    private:
        void run();

        uint64_t worker_id;

        Scheduler& scheduler_ref;

        std::thread worker_thread;

        std::atomic<bool> running;
    };

} // namespace chronos

#endif // CHRONOS_WORKER_H