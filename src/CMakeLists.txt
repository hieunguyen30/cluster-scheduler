add_library(kestrel_core STATIC
  core/cluster_state.cpp
  core/placement_policy.cpp
  core/scheduler.cpp)
target_include_directories(kestrel_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(kestrel_sim_lib STATIC
  sim/workload.cpp
  sim/simulator.cpp)
target_link_libraries(kestrel_sim_lib PUBLIC kestrel_core)

add_executable(kestrel_sim sim/sim_main.cpp)
target_link_libraries(kestrel_sim PRIVATE kestrel_sim_lib)
