#pragma once
#include <cstdint>

namespace Trading {
enum class ProcessingStage : std::uint8_t { MARKET, DECISION, RESPONSE };
struct ProcessingSample {
    ProcessingStage stage;
    // MarketUpdateType, JevIntent, or ClientResponseType, according to stage.
    std::uint32_t kind;
    std::uint64_t elapsed_ns;
    std::uint32_t request_count = 0;
    // Last ClientRequestType emitted by this decision; zero means none.
    std::uint32_t request_type = 0;
};

/// Install before processing; must outlive the engine. Implementations must not
/// throw or mutate trading state. Null observers incur no stage clock reads.
class ProcessingObserver {
  public:
    virtual ~ProcessingObserver() = default;
    virtual auto record(const ProcessingSample& sample) noexcept -> void = 0;
};
}  // namespace Trading
