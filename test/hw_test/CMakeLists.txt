add_executable(test test_main.cc)

target_compile_options(test PRIVATE -pthread)
target_link_options(test PRIVATE -pthread LINKER:--no-as-needed)
target_link_directories(test PRIVATE ${SPDK_lib_directory} ${DPDK_lib_directory})
target_link_libraries(test hscfs 
 spdk_nvme spdk_env_dpdk 
 rte_eal rte_mempool rte_ring rte_bus_pci
 rt uuid ssl crypto m dl
)

target_include_directories(test PRIVATE 
    ${PROJECT_SOURCE_DIR}/inc 
    ${SPDK_include_directory}
)