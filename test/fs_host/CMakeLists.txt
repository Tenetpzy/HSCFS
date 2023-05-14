# add_executable(test_path_utils 
#     test_path_utils.cc
#     ${PROJECT_SOURCE_DIR}/src/fs/path_utils.cc
# )

add_executable(build_fs_image build_fs_image.cc)

# 测试主机侧文件系统，mock通信层
add_library(HscfsTest SHARED)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/cache hscfs_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/fs hscfs_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/journal hscfs_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/utils hscfs_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/api hscfs_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/debug hscfs_srcs)
target_sources(HscfsTest PRIVATE ${hscfs_srcs})
target_sources(HscfsTest PRIVATE 
    ${PROJECT_SOURCE_DIR}/third_party/fmt/src/format.cc
    ${PROJECT_SOURCE_DIR}/test/fs_host/host_test_env.cc
)
target_include_directories(HscfsTest PRIVATE 
    ${PROJECT_SOURCE_DIR}/inc 
    ${SPDK_include_directory}
    ${PROJECT_SOURCE_DIR}/third_party/fmt/include
)
target_compile_options(HscfsTest PRIVATE -DCONFIG_PRINT_DEBUG_INFO)

add_executable(test_read test_read.cc)
target_link_libraries(test_read HscfsTest)
target_include_directories(test_read PRIVATE ${SPDK_include_directory})

add_executable(test_truncate test_truncate.cc)
target_link_libraries(test_truncate HscfsTest)
target_include_directories(test_truncate PRIVATE ${SPDK_include_directory})

add_executable(test_unlink test_unlink.cc)
target_link_libraries(test_unlink HscfsTest)
target_include_directories(test_unlink PRIVATE ${SPDK_include_directory})