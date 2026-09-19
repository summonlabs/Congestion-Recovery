# Congestion Recovery reference processes.

add_executable(crcoordinator crcoordinator.cpp)
target_link_libraries(crcoordinator PRIVATE CongestionRecovery::CongestionRecovery)

add_executable(crworker crworker.cpp)
target_link_libraries(crworker PRIVATE CongestionRecovery::CongestionRecovery)

add_executable(crcli crcli.cpp)
target_link_libraries(crcli PRIVATE CongestionRecovery::CongestionRecovery)

if(MSVC)
  foreach(t crcoordinator crworker crcli)
    target_compile_options(${t} PRIVATE /W4 /WX /MP)
  endforeach()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(t crcoordinator crworker crcli)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endforeach()
endif()
