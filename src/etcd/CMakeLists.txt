compile_library(
  NAME etcd
  SRCS
    etcdapi.cpp
    auth.pb.cc
    auth.grpc.pb.cc
    etcdserver.pb.cc
    etcdserver.grpc.pb.cc
    kv.pb.cc
    kv.grpc.pb.cc
    rpc.grpc.pb.cc
    rpc.pb.cc
  DEPS
    gRPC::gpr
    gRPC::grpc
    gRPC::grpc++
)