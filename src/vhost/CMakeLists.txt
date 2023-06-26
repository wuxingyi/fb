message(STATUS "CMAKE_CURRENT_SOURCE_DIR: " ${CMAKE_CURRENT_SOURCE_DIR})

set (target vhost)
add_executable (vhost
  vhost.cc
)

target_include_directories (vhost
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_compile_options(
  -Wno-error=pointer-arith  # 对void*进行加减运算
  -Wno-error=write-strings  # 把常量字符串当char*使用
)

target_link_libraries (vhost
  PUBLIC
    spdk::spdk
    )

set_target_properties (vhost
  PROPERTIES
    OUTPUT_NAME vhost_demo)
