find_package(spdk REQUIRED)
find_package(absl REQUIRED)
find_package(protobuf REQUIRED CONFIG)
find_package(gRPC CONFIG REQUIRED)

# add_subdirectory(blobkv)
add_subdirectory(spdk)
add_subdirectory(hello_blob)
add_subdirectory(vhost)
add_subdirectory(etcd)
add_subdirectory(osd)
