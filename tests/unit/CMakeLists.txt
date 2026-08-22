find_package(PkgConfig REQUIRED)
pkg_check_modules(CHECK REQUIRED IMPORTED_TARGET check)

add_executable(test_ops test_ops.c)
target_link_libraries(test_ops PRIVATE PkgConfig::CHECK)
target_include_directories(test_ops PRIVATE ${PROJECT_SOURCE_DIR}/../src/include)

add_executable(test_unix_socket_netdev test_unix_socket_netdev.c
               ${PROJECT_SOURCE_DIR}/../src/netdev/netdev.c
               ${PROJECT_SOURCE_DIR}/../src/netdev/netdev_factory.c
               ${PROJECT_SOURCE_DIR}/../src/netdev/impl/unix_socket_netdev.c)
find_package(Threads REQUIRED)
target_link_libraries(test_unix_socket_netdev PRIVATE PkgConfig::CHECK Threads::Threads)
target_include_directories(test_unix_socket_netdev PRIVATE
                           ${PROJECT_SOURCE_DIR}/../src/include
                           ${PROJECT_SOURCE_DIR}/../src/netdev
                           ${PROJECT_SOURCE_DIR}/../src/netdev/impl)
