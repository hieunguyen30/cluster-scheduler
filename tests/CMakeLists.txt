function(kestrel_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE kestrel_sim_lib GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endfunction()

kestrel_test(core_tests cluster_state_test.cpp policies_test.cpp scheduler_test.cpp)
kestrel_test(sim_tests simulator_test.cpp)

# Integration tests: real gRPC, real subprocesses.
function(kestrel_integration_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE kestrel_sim_lib kestrel_control_lib kestrel_worker_lib
                                        GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # Cluster tests are timing-sensitive (heartbeat deadlines): never run two at once, so they do
  # not starve each other's heartbeats. Unit tests still run in parallel.
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60
                       PROPERTIES TIMEOUT 180 RESOURCE_LOCK kestrel_cluster)
endfunction()

kestrel_integration_test(integration_tests integration_test.cpp)
kestrel_integration_test(failure_tests failure_test.cpp)
