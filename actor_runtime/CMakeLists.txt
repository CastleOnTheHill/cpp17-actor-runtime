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

add_library(actor_runtime
    src/actor.cpp
    src/actor_ref.cpp
    src/actor_system.cpp
    src/coroutine_context.cpp
    src/wait.cpp
)
target_include_directories(actor_runtime PUBLIC include)
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

add_executable(request_response_example examples/request_response.cpp)
target_link_libraries(request_response_example PRIVATE actor_runtime)

add_executable(parallel_workflow_example examples/parallel_workflow.cpp)
target_link_libraries(parallel_workflow_example PRIVATE actor_runtime)

add_executable(order_fulfillment_example examples/order_fulfillment.cpp)
target_link_libraries(order_fulfillment_example PRIVATE actor_runtime)
add_test(NAME order_fulfillment COMMAND order_fulfillment_example)
set_tests_properties(order_fulfillment PROPERTIES TIMEOUT 30)

if(ACTOR_RUNTIME_COVERAGE)
    add_custom_target(coverage
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/coverage.py
            --build ${CMAKE_BINARY_DIR} --source ${CMAKE_CURRENT_SOURCE_DIR}
            --gcov ${GCOV_EXECUTABLE} --minimum 90
        DEPENDS actor_runtime_tests wait_semantics_tests api_contract_tests order_fulfillment_example
        USES_TERMINAL VERBATIM)
endif()
