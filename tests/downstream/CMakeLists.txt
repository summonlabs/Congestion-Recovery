# Independent find_package consumer. Built as a separate CMake project so the
# installed package is exercised exactly as a third party would use it.
add_executable(cr_downstream main.cpp)
target_link_libraries(cr_downstream PRIVATE CongestionRecovery::CongestionRecovery)
if(MSVC)
  target_compile_options(cr_downstream PRIVATE /W4 /WX /MP)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cr_downstream PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME cr_downstream COMMAND cr_downstream)
