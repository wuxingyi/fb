add_library(etcdapi
    etcdapi.cpp
    auth.pb.cc
    auth.grpc.pb.cc
    etcdserver.pb.cc
    etcdserver.grpc.pb.cc
    kv.pb.cc
    kv.grpc.pb.cc
    rpc.grpc.pb.cc
    rpc.pb.cc)

target_link_libraries(etcdapi
  PRIVATE
    # We do not want the gRPC libraries publicly declared
    # since we do not use it in the interface.
    gRPC::gpr
    gRPC::grpc
    gRPC::grpc++
    )

set_target_properties(etcdapi PROPERTIES CXX_STANDARD 17)

add_executable(Tests
     test.cpp)

target_link_libraries(Tests
       etcdapi
       )

set_target_properties(Tests PROPERTIES CXX_STANDARD 17)

