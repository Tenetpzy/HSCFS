set(test_lib_srcs 
    ${PROJECT_SOURCE_DIR}/src/communication/channel.c 
    ${PROJECT_SOURCE_DIR}/src/communication/comm_api.c 
    ${PROJECT_SOURCE_DIR}/src/communication/session.c 
    ${PROJECT_SOURCE_DIR}/src/utils/hscfs_log.c 
    ${PROJECT_SOURCE_DIR}/src/utils/hscfs_timer.c
)

add_executable(test_comm)
target_sources(test_comm PRIVATE 
    test_comm.cc
    spdk_mock.cc
    ${test_lib_srcs}
)
target_include_directories(test_comm PUBLIC
    ${SPDK_include_directory}
)

# add_executable(test_docking)
# target_sources(test_docking PRIVATE
#     test_docking.cc
#     spdk_mock.cc
#     ${test_lib_srcs}
# )
# target_include_directories(test_docking PUBLIC
#     ${SPDK_include_directory}
# )