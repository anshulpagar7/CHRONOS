// Day 1 demo: the foundation layer end to end.
// Submits jobs through the JobStore, walks them through their lifecycle,
// and lets the EventLogger render every emitted event as structured logs.

#include <chrono>

#include "chronos/core/clock.h"
#include "chronos/core/event_bus.h"
#include "chronos/core/job_store.h"
#include "chronos/observability/event_logger.h"
#include "chronos/observability/log.h"

using namespace chronos;
using namespace std::chrono_literals;

int main() {
    SystemClock clock;
    EventBus bus;
    JobStore store(clock, bus);
    EventLogger logger(bus);  // Every event below becomes a log line.

    log::info("chronos day-1 demo starting");

    // Job 1: clean run.
    const JobId a = store.submit(
        JobSpecBuilder{}.name("encode-video").priority(5).max_retries(3).build());
    store.transition(a, JobState::Queued);
    store.transition(a, JobState::Dispatched);
    store.transition(a, JobState::Running);
    store.transition(a, JobState::Completed);

    // Job 2: fails once, retries, succeeds.
    const JobId b = store.submit(
        JobSpecBuilder{}.name("train-model").priority(8).max_retries(2).build());
    store.transition(b, JobState::Queued);
    store.transition(b, JobState::Dispatched);
    store.transition(b, JobState::Running);
    store.transition(b, JobState::RetryWait);
    store.transition(b, JobState::Queued);
    store.transition(b, JobState::Dispatched);
    store.transition(b, JobState::Running);
    store.transition(b, JobState::Completed);

    // Job 3: cancelled while queued.
    const JobId c = store.submit(JobSpecBuilder{}.name("nightly-report").build());
    store.transition(c, JobState::Queued);
    store.transition(c, JobState::Cancelled);

    // The state machine rejects nonsense loudly.
    try {
        store.transition(a, JobState::Running);  // a is Completed: illegal.
    } catch (const IllegalTransitionError& e) {
        log::warn("state machine rejected transition", {{"what", e.what()}});
    }

    log::info("demo finished",
              {{"jobs", static_cast<std::uint64_t>(store.size())},
               {"completed", static_cast<std::uint64_t>(store.count(JobState::Completed))},
               {"cancelled", static_cast<std::uint64_t>(store.count(JobState::Cancelled))}});
    return 0;
}
