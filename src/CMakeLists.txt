# ---- pure core + simulator (no gRPC dependency) -------------------------------------------
add_library(kestrel_core STATIC
  core/cluster_state.cpp
  core/placement_policy.cpp
  core/scheduler.cpp)
target_include_directories(kestrel_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(kestrel_common STATIC common/log.cpp)
target_include_directories(kestrel_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(kestrel_wal STATIC wal/wal.cpp)
target_link_libraries(kestrel_wal PUBLIC kestrel_core kestrel_common)

add_library(kestrel_sim_lib STATIC
  sim/workload.cpp
  sim/simulator.cpp)
target_link_libraries(kestrel_sim_lib PUBLIC kestrel_core)

add_executable(kestrel_sim sim/sim_main.cpp)
target_link_libraries(kestrel_sim PRIVATE kestrel_sim_lib)

# ---- protobuf / gRPC code generation ------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++ protobuf)
find_program(PROTOC protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

set(PROTO_FILE ${PROJECT_SOURCE_DIR}/proto/kestrel.proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
add_custom_command(
  OUTPUT ${PROTO_GEN_DIR}/kestrel.pb.cc ${PROTO_GEN_DIR}/kestrel.pb.h
         ${PROTO_GEN_DIR}/kestrel.grpc.pb.cc ${PROTO_GEN_DIR}/kestrel.grpc.pb.h
  COMMAND ${PROTOC}
          --proto_path=${PROJECT_SOURCE_DIR}/proto
          --cpp_out=${PROTO_GEN_DIR}
          --grpc_out=${PROTO_GEN_DIR}
          --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
          ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
  COMMENT "Generating protobuf/gRPC code")

add_library(kestrel_proto STATIC
  ${PROTO_GEN_DIR}/kestrel.pb.cc ${PROTO_GEN_DIR}/kestrel.grpc.pb.cc)
target_include_directories(kestrel_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(kestrel_proto PUBLIC PkgConfig::GRPCPP)
# generated code triggers pedantic warnings we do not own
target_compile_options(kestrel_proto PRIVATE -w)

# ---- control plane, worker, CLI ----------------------------------------------------------
add_library(kestrel_control_lib STATIC
  control/control_plane.cpp
  control/grpc_server.cpp)
target_link_libraries(kestrel_control_lib PUBLIC kestrel_core kestrel_common kestrel_wal kestrel_proto)

add_library(kestrel_worker_lib STATIC worker/worker_agent.cpp)
target_link_libraries(kestrel_worker_lib PUBLIC kestrel_core kestrel_common kestrel_proto)

add_executable(kestrel_control control/control_plane_main.cpp)
target_link_libraries(kestrel_control PRIVATE kestrel_control_lib)

add_executable(kestrel_worker worker/worker_main.cpp)
target_link_libraries(kestrel_worker PRIVATE kestrel_worker_lib)

add_executable(kestrelctl cli/kestrelctl.cpp)
target_link_libraries(kestrelctl PRIVATE kestrel_proto)
target_include_directories(kestrelctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
