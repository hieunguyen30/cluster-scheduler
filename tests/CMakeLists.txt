function(kestrel_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE kestrel_sim_lib GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endfunction()

kestrel_test(core_tests cluster_state_test.cpp policies_test.cpp scheduler_test.cpp)
kestrel_test(sim_tests simulator_test.cpp)
kestrel_test(wal_tests wal_test.cpp)
target_link_libraries(wal_tests PRIVATE kestrel_wal)

# Integration tests: real gRPC, real subprocesses.
function(kestrel_integration_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE kestrel_sim_lib kestrel_control_lib kestrel_worker_lib
                                        GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # Cluster tests are timing-sensitive (heartbeat deadlines): never run two at once, so they do
  # not starve each other's heartbeats. Unit tests still run in parallel.
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60
                       PROPERTIES TIMEOUT 180 RESOURCE_LOCK kestrel_cluster)
endfunction()

kestrel_integration_test(integration_tests integration_test.cpp)
kestrel_integration_test(failure_tests failure_test.cpp)
kestrel_integration_test(restart_tests restart_test.cpp)
kestrel_integration_test(metrics_tests metrics_test.cpp)
kestrel_integration_test(control_plane_tests control_plane_test.cpp)
kestrel_integration_test(reconnect_tests reconnect_test.cpp)
