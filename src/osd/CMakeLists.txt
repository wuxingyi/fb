add_executable(Tests
     test.cpp)

target_link_libraries(Tests
    PUBLIC
       fastblock_etcd
       )


