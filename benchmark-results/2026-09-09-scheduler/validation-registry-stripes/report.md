# WSL GCC coverage

all runtime src/include C++ files; no exclusions

| File | Covered / executable lines | Line coverage |
| --- | ---: | ---: |
| include/actor_runtime/actor_ref.h | 1 / 1 | 100.00% |
| include/actor_runtime/actor_system.h | 1 / 1 | 100.00% |
| include/actor_runtime/wait.h | 2 / 2 | 100.00% |
| src/actor.cpp | 12 / 12 | 100.00% |
| src/actor_ref.cpp | 7 / 7 | 100.00% |
| src/actor_system.cpp | 798 / 822 | 97.08% |
| src/blocking_pool.cpp | 26 / 27 | 96.30% |
| src/c_api.cpp | 95 / 97 | 97.94% |
| src/coroutine_context.cpp | 61 / 63 | 96.83% |
| src/internal.h | 9 / 10 | 90.00% |
| src/ipc_transport.cpp | 108 / 109 | 99.08% |
| src/sanitizer.h | 4 / 4 | 100.00% |
| src/wait.cpp | 52 / 52 | 100.00% |
| src/wait_internal.h | 2 / 2 | 100.00% |

Line coverage: **1178/1209 = 97.44%** (gate >= 90%).
Raw gcov branch coverage: 1197/1918 = 62.41% (includes C++ exception edges).
