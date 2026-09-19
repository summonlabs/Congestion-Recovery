# Congestion Recovery tests. No test timeouts anywhere.
#
# Multiprocess suites drive real crcoordinator/crworker OS processes over
# framed TCP; they are registered only when the tools are part of the build.

set(CR_TEST_TARGETS
  cr_smoke
  cr_stages
  cr_rollback
  cr_obligations
  cr_persistence
  cr_adversarial
  cr_property
  cr_concurrency
)

add_executable(cr_smoke smoke.cpp)
add_executable(cr_stages stages.cpp)
add_executable(cr_rollback rollback.cpp)
add_executable(cr_obligations obligations.cpp)
add_executable(cr_persistence persistence.cpp)
add_executable(cr_adversarial adversarial.cpp)
add_executable(cr_property property.cpp)
add_executable(cr_concurrency concurrency.cpp)

foreach(t ${CR_TEST_TARGETS})
  target_link_libraries(${t} PRIVATE CongestionRecovery::CongestionRecovery)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${t} COMMAND ${t})
endforeach()

if(MSVC)
  foreach(t ${CR_TEST_TARGETS})
    target_compile_options(${t} PRIVATE /W4 /WX /MP)
  endforeach()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(t ${CR_TEST_TARGETS})
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endforeach()
endif()

if(CR_ENABLE_ASAN AND MSVC)
  foreach(t ${CR_TEST_TARGETS})
    target_compile_options(${t} PRIVATE /fsanitize=address /Zi)
    target_link_options(${t} PRIVATE /fsanitize=address)
  endforeach()
endif()

find_program(CR_POWERSHELL_EXEC powershell)
if(CR_POWERSHELL_EXEC AND CR_BUILD_TOOLS)
  add_test(NAME cr_multiprocess
    COMMAND ${CR_POWERSHELL_EXEC} -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/multiprocess.ps1
            -ToolsDir ${CMAKE_BINARY_DIR}/tools -WorkDir ${CMAKE_BINARY_DIR}/mp)
  add_test(NAME cr_coordinator_restart
    COMMAND ${CR_POWERSHELL_EXEC} -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/coordinator_restart.ps1
            -ToolsDir ${CMAKE_BINARY_DIR}/tools -WorkDir ${CMAKE_BINARY_DIR}/restart)
endif()

# Independent downstream consumer validating find_package closure.
add_subdirectory(downstream)
