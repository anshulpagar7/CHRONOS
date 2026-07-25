#pragma once

#include "chronos/core/clock.h"
#include "chronos/core/job.h"

namespace chronos {

/// Strategy interface for ranking jobs.
///
/// A policy reduces a job (at a given instant) to a single scalar score;
/// the scheduler always prefers the highest-scoring eligible job. Workers
/// and the ReadySet know nothing about *why* a job ranks where it does,
/// so policies can be swapped -- or A/B compared in the simulator --
/// without touching any other component.
///
/// Contract:
///  * Higher score == schedule sooner.
///  * Pure function of (job, now): no side effects, no hidden state.
///    Determinism here is what makes simulator runs reproducible.
///  * Scores are only compared against scores from the same policy at the
///    same `now`; absolute magnitudes carry no meaning across policies.
class SchedulingPolicy {
public:
    virtual ~SchedulingPolicy() = default;

    [[nodiscard]] virtual double score(const Job& job, TimePoint now) const = 0;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

}  // namespace chronos
