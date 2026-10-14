#pragma once
#include <array>
#include <cstdint>

namespace Common {
// Local observation metadata only; never serialize into a venue/provider request.
struct PerformanceTrace {
    std::array<std::uint64_t, 8> t{};
    std::uint64_t source_id = 0;
    std::uint64_t evaluation_id = 0;
};
}
