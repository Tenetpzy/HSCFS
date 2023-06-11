cmake_minimum_required(VERSION 3.0)

project(HSCFS)

set(SPDK_include_directory /home/pzy/software/spdk/build/include)
set(SPDK_lib_directory /home/pzy/software/spdk/build/lib)
set(DPDK_lib_directory /home/pzy/software/spdk/dpdk/build/lib)

add_subdirectory(third_party)
add_subdirectory(test)
add_subdirectory(src)
add_subdirectory(mkfs)