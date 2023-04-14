include_directories(
    ${PROJECT_SOURCE_DIR}/inc 
    ${PROJECT_SOURCE_DIR}/third_party/fmt/include
    ${PROJECT_SOURCE_DIR}/test/inc
)
link_directories(/usr/local/lib)

add_compile_options(-g3 -O0 -Wall)
# add_compile_options(-DHSCFS_DEBUG)
link_libraries(gtest pthread)

add_subdirectory(utils)
add_subdirectory(communication)
add_subdirectory(journal)
add_subdirectory(cache)
add_subdirectory(fs_basic_modules)