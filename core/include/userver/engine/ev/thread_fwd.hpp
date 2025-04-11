#pragma once

#include <chrono>

USERVER_NAMESPACE_BEGIN

namespace engine::ev {

// Avoid ev_async_send on timers that have bigger timeouts
inline constexpr std::chrono::microseconds kMinDurationToDefer{19500};

}  // namespace engine::ev

USERVER_NAMESPACE_END
