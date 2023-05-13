# 当前为debug版，包含了fmt库

aux_source_directory(${PROJECT_SOURCE_DIR}/src/cache hscfs_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/communication hscfs_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/fs hscfs_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/journal hscfs_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/utils hscfs_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/api hscfs_srcs)

target_sources(hscfs PRIVATE ${hscfs_srcs})
target_include_directories(hscfs PRIVATE 
    ${PROJECT_SOURCE_DIR}/inc 
    ${SPDK_include_directory}
)

if(NOT DEFINED CONFIG_PRINT_DEBUG_INFO)
    set(CONFIG_PRINT_DEBUG_INFO No)
endif()

if(${CONFIG_PRINT_DEBUG_INFO} STREQUAL Yes)
    message("CONFIG_PRINT_DEBUG_INFO=Yes")
    target_include_directories(hscfs PRIVATE ${PROJECT_SOURCE_DIR}/third_party/fmt/include)
    target_sources(hscfs PRIVATE 
        ${PROJECT_SOURCE_DIR}/third_party/fmt/src/format.cc
        ${PROJECT_SOURCE_DIR}/src/debug/debug_utils.cc
    )
    target_compile_options(hscfs PRIVATE -DCONFIG_PRINT_DEBUG_INFO)
endif()