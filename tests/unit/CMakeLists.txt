find_package(PkgConfig REQUIRED)
pkg_check_modules(CHECK REQUIRED IMPORTED_TARGET check)

add_executable(test_ops test_ops.c)
target_link_libraries(test_ops PRIVATE PkgConfig::CHECK)
target_include_directories(test_ops PRIVATE ${PROJECT_SOURCE_DIR}/../src/include)

add_executable(test_hubport test_hubport.c
               ${PROJECT_SOURCE_DIR}/../src/netdev/netdev.c
               ${PROJECT_SOURCE_DIR}/../src/netdev/hubport.c)
target_link_libraries(test_hubport PRIVATE PkgConfig::CHECK)
target_include_directories(test_hubport PRIVATE
                           ${PROJECT_SOURCE_DIR}/../src/include
                           ${PROJECT_SOURCE_DIR}/../src/netdev)
