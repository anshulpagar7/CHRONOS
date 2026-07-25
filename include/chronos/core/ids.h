#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace chronos {

/// Strongly-typed integral identifier.
///
/// Prevents accidentally passing a WorkerId where a JobId is expected --
/// a class of bug the compiler can eliminate for free. The Tag parameter
/// is never instantiated; it exists only to make each alias a distinct type.
///
/// Id value 0 is reserved as "invalid / unset".
template <typename Tag>
class StrongId {
public:
    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    constexpr auto operator<=>(const StrongId&) const noexcept = default;

private:
    std::uint64_t value_ = 0;
};

using JobId    = StrongId<struct JobIdTag>;
using WorkerId = StrongId<struct WorkerIdTag>;

}  // namespace chronos

// Hash support so StrongIds can key unordered containers.
template <typename Tag>
struct std::hash<chronos::StrongId<Tag>> {
    std::size_t operator()(const chronos::StrongId<Tag>& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};
