add_executable(mkhscfs)

target_sources(mkhscfs PRIVATE
    ${PROJECT_SOURCE_DIR}/mkfs/mkhscfs.cc
)

target_compile_options(mkhscfs PRIVATE -pthread)
target_link_options(mkhscfs PRIVATE -pthread LINKER:--no-as-needed)
target_link_directories(mkhscfs PRIVATE ${SPDK_lib_directory} ${DPDK_lib_directory})
target_link_libraries(mkhscfs hscfs 
 spdk_nvme spdk_env_dpdk 
 rte_eal rte_mempool rte_ring rte_bus_pci
 rt uuid ssl crypto m dl
)

target_include_directories(mkhscfs PRIVATE 
    ${PROJECT_SOURCE_DIR}/inc 
    ${SPDK_include_directory}
)