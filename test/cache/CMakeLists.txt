add_executable(test_cache_manager 
    test_cache_manager.cc
    ${PROJECT_SOURCE_DIR}/src/utils/hscfs_log.c
)

add_executable(test_sit_nat_cache
    test_sit_nat_cache.cc
    cache_mock.cc
    ${PROJECT_SOURCE_DIR}/src/utils/hscfs_log.c
    ${PROJECT_SOURCE_DIR}/src/cache/block_buffer.cc
    ${PROJECT_SOURCE_DIR}/src/cache/SIT_NAT_cache.cc
)
target_include_directories(test_sit_nat_cache PRIVATE
    ${SPDK_include_directory}
)
target_link_libraries(test_sit_nat_cache fmt)

add_executable(test_node_cache
    test_node_cache.cc
    cache_mock.cc
    ${PROJECT_SOURCE_DIR}/src/utils/hscfs_log.c
    ${PROJECT_SOURCE_DIR}/src/cache/block_buffer.cc
    ${PROJECT_SOURCE_DIR}/src/cache/node_block_cache.cc
)
target_include_directories(test_node_cache PRIVATE
    ${SPDK_include_directory}
)