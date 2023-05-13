add_executable(test_journal
    test_journal.cc
    ${PROJECT_SOURCE_DIR}/src/journal/journal_process_env.cc
    ${PROJECT_SOURCE_DIR}/src/journal/journal_processor.cc
    ${PROJECT_SOURCE_DIR}/src/journal/journal_writer.cc
    ${PROJECT_SOURCE_DIR}/src/utils/hscfs_log.c
    ${PROJECT_SOURCE_DIR}/src/utils/hscfs_timer.c
    ${PROJECT_SOURCE_DIR}/src/cache/block_buffer.cc
    ${PROJECT_SOURCE_DIR}/src/debug/debug_utils.cc
)

target_link_libraries(test_journal fmt)
target_include_directories(test_journal PUBLIC
    ${SPDK_include_directory}
)
target_compile_options(test_journal PRIVATE -DCONFIG_PRINT_DEBUG_INFO)