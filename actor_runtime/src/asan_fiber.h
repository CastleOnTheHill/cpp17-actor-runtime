#pragma once

#include <cstddef>

namespace actor_runtime {
namespace detail {

// AddressSanitizer 的协程（fiber）栈切换注解接口。
//
// 这里把两个 hook 声明成弱符号：进程链接了 ASan 运行时（-fsanitize=address）
// 时它们解析到真实实现，否则为 nullptr。因此运行库不需要在编译期依赖 ASan，
// 未启用 ASan 的构建会安全地退化为空操作。
extern "C" {
void __sanitizer_start_switch_fiber(void** fakeStackSave, const void* bottom, std::size_t size)
    __attribute__((weak));
void __sanitizer_finish_switch_fiber(void* fakeStackSave, const void** bottomOld, std::size_t* sizeOld)
    __attribute__((weak));
}

// 进程内是否存在 ASan 的 fiber hook 实现。
bool asanFiberHooksAvailable() noexcept;

// 切换栈之前调用。targetBottom/targetSize 描述目标栈的范围。
// 当前栈上一次 asanStartSwitch 保存的值会在它恢复运行时交给 asanFinishSwitch。
// fakeStackSave 传 nullptr 表示“彻底离开当前栈”，其 fake stack 会被销毁。
void asanStartSwitch(void** fakeStackSave, const void* targetBottom, std::size_t targetSize) noexcept;

// 在恢复运行的栈上紧接着调用，入参是本栈上一次 asanStartSwitch 保存的值。
void asanFinishSwitch(void* fakeStackSave) noexcept;

} // namespace detail
} // namespace actor_runtime
