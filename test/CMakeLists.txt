include_directories(../inc inc)
link_directories(/usr/local/lib)

add_compile_options(-g -Wall)
link_libraries(gtest pthread)

add_subdirectory(utils)