include_directories(${PROJECT_SOURCE_DIR}/inc ${PROJECT_SOURCE_DIR}/test/inc)
link_directories(/usr/local/lib)

add_compile_options(-g -Wall -DHSCFS_DEBUG)
link_libraries(gtest pthread)

add_subdirectory(utils)
add_subdirectory(communication)