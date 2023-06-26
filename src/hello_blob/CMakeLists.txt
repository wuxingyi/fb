# hello_blob 

add_executable(hello_blob
  hello_blob.cc
  )

target_link_libraries(hello_blob 
  PUBLIC 
    spdk::spdk
)

target_include_directories (hello_blob
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

#[[
set_property(TARGET hello_blob PROPERTY POSITION_INDEPENDENT_CODE ON)
if(CMAKE_BUILD_TYPE MATCHES Release)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ltosupported OUTPUT error)
  if(ltosupported)
    set_property(TARGET hello_blob PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
  endif()
endif()
]]
