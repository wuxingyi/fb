add_library(sunyifang_spdk STATIC
  empty.cc)

target_link_libraries (sunyifang_spdk
  PUBLIC
    spdk::spdk)