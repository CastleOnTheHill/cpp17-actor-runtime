#include "asan_fiber.h"

namespace actor_runtime {
namespace detail {

bool asanFiberHooksAvailable() noexcept {
    return __sanitizer_start_switch_fiber != nullptr && __sanitizer_finish_switch_fiber != nullptr;
}

void asanStartSwitch(void** fakeStackSave, const void* targetBottom, std::size_t targetSize) noexcept {
    if (__sanitizer_start_switch_fiber == nullptr) return;
    __sanitizer_start_switch_fiber(fakeStackSave, targetBottom, targetSize);
}

void asanFinishSwitch(void* fakeStackSave) noexcept {
    if (__sanitizer_finish_switch_fiber == nullptr) return;
    __sanitizer_finish_switch_fiber(fakeStackSave, nullptr, nullptr);
}

} // namespace detail
} // namespace actor_runtime
