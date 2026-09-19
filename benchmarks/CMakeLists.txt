# Congestion Recovery benchmarks. Synthetic populations only.

add_executable(cr_bench_recovery recovery_bench.cpp)
target_link_libraries(cr_bench_recovery PRIVATE CongestionRecovery::CongestionRecovery)

if(MSVC)
  target_compile_options(cr_bench_recovery PRIVATE /W4 /WX /MP)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cr_bench_recovery PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
