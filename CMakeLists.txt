cmake_minimum_required(VERSION 3.0)

project(HSCFS)

set(SPDK_include_directory /home/pzy/software/spdk/build/include)

add_library(hscfs SHARED)

add_subdirectory(third_party)
add_subdirectory(test)
add_subdirectory(src)