add_executable(test_comm)
target_sources(test_comm PRIVATE 
    test_comm.cc 
    spdk_mock.cc
    ${PROJECT_SOURCE_DIR}/src/communication/channel.c 
    ${PROJECT_SOURCE_DIR}/src/communication/comm_api.c 
    ${PROJECT_SOURCE_DIR}/src/communication/session.c 
    ${PROJECT_SOURCE_DIR}/src/utils/hscfs_log.c 
    ${PROJECT_SOURCE_DIR}/src/utils/hscfs_timer.c 
)
target_include_directories(test_comm PUBLIC
    /home/pzy/spdk_src/spdk/build/include
)