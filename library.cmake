include(CMakeParseArguments)

set(CXX_STANDARD 20)
set(DEFAULT_LINKOPTS)
set(DEFAULT_COPTS -Wall -Wextra -Werror -Wno-missing-field-initializers)
set(COMMON_INCLUDE_DIRS
  "${PROJECT_SOURCE_DIR}/src/")
function(compile_library)
  cmake_parse_arguments(CC_LIB
    "DISABLE_INSTALL;PUBLIC"
    "NAME"
    "HDRS;SRCS;COPTS;DEFINES;LINKOPTS;DEPS"
    ${ARGN}
    )

  set(_NAME "fblock_${CC_LIB_NAME}")

  # Check if this is a header-only library
  # Note that as of February 2019, many popular OS's (for example, Ubuntu
  # 16.04 LTS) only come with cmake 3.5 by default.  For this reason, we can't
  # use list(FILTER...)
  set(CC_SRCS "${CC_LIB_SRCS}")
  foreach(src_file IN LISTS CC_SRCS)
    if(${src_file} MATCHES ".*\\.(h|inc)")
      list(REMOVE_ITEM CC_SRCS "${src_file}")
    endif()
  endforeach()
  if("${CC_SRCS}" STREQUAL "")
    set(CC_LIB_IS_INTERFACE 1)
  else()
    set(CC_LIB_IS_INTERFACE 0)
  endif()

  if(NOT CC_LIB_IS_INTERFACE)
    # CMake creates static libraries in anything non Debug mode.
    if ("${CMAKE_BUILD_TYPE}" MATCHES "Debug")
      # TODO - enable once we fix seastar scalability exception hack
      # add_library(${_NAME} SHARED "")
      add_library(${_NAME} "")
    else()
      # allow override on the command line for -DBUILD_SHARED_LIBS=ON
      add_library(${_NAME} "")
    endif()
    target_sources(${_NAME} PRIVATE ${CC_LIB_SRCS} ${CC_LIB_HDRS})
    target_include_directories(${_NAME}
      PUBLIC
      "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
      # we don't install targets
      # $<INSTALL_INTERFACE:${INSTALL_INCLUDEDIR}>
      )
    target_compile_options(${_NAME}
      PRIVATE ${DEFAULT_COPTS}
      PRIVATE ${CC_LIB_COPTS})
    target_link_libraries(${_NAME}
      PUBLIC ${CC_LIB_DEPS}
      PRIVATE
      ${CC_LIB_LINKOPTS}
      ${DEFAULT_LINKOPTS}
      )
    target_compile_definitions(${_NAME} PUBLIC ${CC_LIB_DEFINES})
    # INTERFACE libraries can't have the CXX_STANDARD property set
    set_property(TARGET ${_NAME} PROPERTY CXX_STANDARD ${CXX_STANDARD})
    set_property(TARGET ${_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)

    set_target_properties(${_NAME} PROPERTIES
      OUTPUT_NAME "${_NAME}"
      )
  else()
    # Generating header-only library
    add_library(${_NAME} INTERFACE)
    target_include_directories(${_NAME}
      INTERFACE
      "$<BUILD_INTERFACE:${COMMON_INCLUDE_DIRS}>"
      # We don't install
      #$<INSTALL_INTERFACE:${INSTALL_INCLUDEDIR}>
      )
    target_link_libraries(${_NAME}
      INTERFACE
      ${CC_LIB_DEPS}
      ${CC_LIB_LINKOPTS}
      ${DEFAULT_LINKOPTS}
      )
    target_compile_definitions(${_NAME} INTERFACE ${CC_LIB_DEFINES})
  endif()
  # main symbol exported
  add_library(${CC_LIB_NAME} ALIAS ${_NAME})
endfunction()
