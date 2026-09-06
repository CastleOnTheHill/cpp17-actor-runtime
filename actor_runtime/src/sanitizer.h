#pragma once

// 使用真实的 ASan fiber-switch 协议；普通构建无第三方运行时依赖。
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/common_interface_defs.h>
#endif
#include <cstddef>
namespace actor_runtime {
inline void sanitizerStart(void** saved, const void* bottom, std::size_t size) {
#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_start_switch_fiber(saved, bottom, size);
#else
    (void)saved; (void)bottom; (void)size;
#endif
}
inline void sanitizerFinish(void* saved, const void** bottom = nullptr, std::size_t* size = nullptr) {
#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_finish_switch_fiber(saved, bottom, size);
#else
    (void)saved; (void)bottom; (void)size;
#endif
}
}
