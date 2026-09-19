# Congestion Recovery examples.

add_executable(cr_example_staged staged_recovery.cpp)
target_link_libraries(cr_example_staged PRIVATE CongestionRecovery::CongestionRecovery)

add_executable(cr_example_recurrence recurrence_and_rollback.cpp)
target_link_libraries(cr_example_recurrence PRIVATE CongestionRecovery::CongestionRecovery)

if(MSVC)
  foreach(t cr_example_staged cr_example_recurrence)
    target_compile_options(${t} PRIVATE /W4 /WX /MP)
  endforeach()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(t cr_example_staged cr_example_recurrence)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endforeach()
endif()
