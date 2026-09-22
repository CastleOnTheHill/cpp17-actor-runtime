cmake_minimum_required(VERSION 3.16)
project(actor_runtime LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ACTOR_RUNTIME_COVERAGE "Instrument the runtime and enforce >=90% line coverage" OFF)
if(ACTOR_RUNTIME_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "Coverage tooling requires GCC/gcov")
    endif()
    add_compile_options(-O0 -g --coverage -fprofile-update=atomic)
    add_link_options(--coverage)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(GCOV_EXECUTABLE gcov REQUIRED)
endif()

# 验证 ASan 协程切换 hook：把运行库和用例一起插桩，并在运行时打开
# detect_stack_use_after_return，让 fake stack 真正参与协程栈切换。
option(ACTOR_RUNTIME_ASAN "Build the runtime and tests with AddressSanitizer" OFF)
if(ACTOR_RUNTIME_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

add_library(actor_runtime
    src/actor.cpp
    src/actor_ref.cpp
    src/actor_system.cpp
    src/asan_fiber.cpp
    src/coroutine_context.cpp
    src/wait.cpp
)
target_include_directories(actor_runtime PUBLIC include)
target_compile_definitions(actor_runtime PRIVATE _GNU_SOURCE)
target_compile_options(actor_runtime PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_link_libraries(actor_runtime PUBLIC Threads::Threads)

enable_testing()
add_executable(actor_runtime_tests tests/runtime_tests.cpp)
target_link_libraries(actor_runtime_tests PRIVATE actor_runtime)
add_test(NAME actor_runtime_tests COMMAND actor_runtime_tests)
set_tests_properties(actor_runtime_tests PROPERTIES TIMEOUT 30)

add_executable(wait_semantics_tests tests/wait_semantics_tests.cpp)
target_link_libraries(wait_semantics_tests PRIVATE actor_runtime)
add_test(NAME wait_semantics_tests COMMAND wait_semantics_tests)
set_tests_properties(wait_semantics_tests PROPERTIES TIMEOUT 30)

add_executable(api_contract_tests tests/api_contract_tests.cpp)
target_link_libraries(api_contract_tests PRIVATE actor_runtime)
add_test(NAME api_contract_tests COMMAND api_contract_tests)
set_tests_properties(api_contract_tests PROPERTIES TIMEOUT 30)

add_executable(actor_system_lifecycle_tests tests/actor_system_lifecycle_tests.cpp)
target_link_libraries(actor_system_lifecycle_tests PRIVATE actor_runtime)
add_test(NAME actor_system_lifecycle_tests COMMAND actor_system_lifecycle_tests)
set_tests_properties(actor_system_lifecycle_tests PROPERTIES TIMEOUT 30)

add_executable(asan_fiber_tests tests/asan_fiber_tests.cpp)
target_link_libraries(asan_fiber_tests PRIVATE actor_runtime)
add_test(NAME asan_fiber_tests COMMAND asan_fiber_tests)
set_tests_properties(asan_fiber_tests PROPERTIES TIMEOUT 60 ENVIRONMENT
    "ASAN_OPTIONS=detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=0:detect_leaks=1")

add_executable(request_response_example examples/request_response.cpp)
target_link_libraries(request_response_example PRIVATE actor_runtime)

add_executable(parallel_workflow_example examples/parallel_workflow.cpp)
target_link_libraries(parallel_workflow_example PRIVATE actor_runtime)

add_executable(order_fulfillment_example examples/order_fulfillment.cpp)
target_link_libraries(order_fulfillment_example PRIVATE actor_runtime)
add_test(NAME order_fulfillment COMMAND order_fulfillment_example)
set_tests_properties(order_fulfillment PROPERTIES TIMEOUT 30)

add_executable(module_registration_example
    examples/module_registration.cpp
    examples/modules/inventory_module.cpp
    examples/modules/order_module.cpp
)
target_include_directories(module_registration_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
target_link_libraries(module_registration_example PRIVATE actor_runtime)
add_test(NAME module_registration COMMAND module_registration_example)
set_tests_properties(module_registration PROPERTIES TIMEOUT 30)

if(ACTOR_RUNTIME_COVERAGE)
    add_custom_target(coverage
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/coverage.py
            --build ${CMAKE_BINARY_DIR} --source ${CMAKE_CURRENT_SOURCE_DIR}
            --gcov ${GCOV_EXECUTABLE} --minimum 90
        DEPENDS actor_runtime_tests wait_semantics_tests api_contract_tests
            actor_system_lifecycle_tests asan_fiber_tests order_fulfillment_example
            module_registration_example
        USES_TERMINAL VERBATIM)
endif()
