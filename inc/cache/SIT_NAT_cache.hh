#pragma once

#include "cache/cache_manager.hh"
#include "cache/block_buffer.hh"
#include "fs/fs.h"

struct comm_dev;

namespace hscfs {

class SIT_NAT_cache_entry
{
public:
    SIT_NAT_cache_entry(uint32_t lpa) {
        lpa_ = lpa;
        ref_count = 0;
    }

    /* 同步从SSD读取lpa到cache中 */
    void read_content(comm_dev *dev);

    uint32_t get_lpa() const noexcept {
        return lpa_;
    }
    
    uint32_t get_ref_count() const noexcept {
        return ref_count;
    }

    void add_ref_count() noexcept {
        ++ref_count;
    }

    void sub_ref_count() noexcept {
        --ref_count;
    } 

    /* 
     * 将缓存块地址转化成 hscfs_sit_block* 或 hscfs_nat_block* 返回
     * 作为sit cache时，应使用get_cache_ptr_in_sit，作为nat cache时使用另一个
     */
    hscfs_sit_block *get_cache_ptr_in_sit() noexcept {
        return reinterpret_cast<hscfs_sit_block*>(cache.get_ptr());
    }

    hscfs_nat_block *get_cache_ptr_in_nat() noexcept {
        return reinterpret_cast<hscfs_nat_block*>(cache.get_ptr());
    }

private:
    uint32_t lpa_;
    uint32_t ref_count;
    block_buffer cache;
};

/* 
 * SIT、NAT缓存控制器
 * 以lpa(uint32_t)作为key，SIT_NAT_cache_entry作为缓存项 
 */
class SIT_NAT_cache
{
public:
    /* 
     * expect_cache_size：期望的缓存大小，即期望系统运行时缓存中能容纳的缓存项的最大个数
     *
     * “期望”：HSCFS不保证缓存项个数一定在期望个数内，原因：
     * 1. 缓存项有可能因为淘汰保护等原因被pin住，无法被置换，因此限制缓存项最大个数时，
     * 无法保证系统运行时一定能获取到缓存资源。（缓存项不会被pin住时，一定可以通过置换获取到缓存资源）
     * 2. 此版本HSCFS运行时无法提供回滚机制，因为只记录文件系统元数据日志，而没有记录目录文件修改的日志。
     * 事务（某个API）运行时，一旦发生资源不足，无法回滚，也就无法向应用程序返回ENOMEM错误，
     * 为了文件系统一致性，不可以写入修改过的数据和元数据，只能终止进程。
     * 
     * 为了尽量防止上述进程终止的发生，HSCFS允许缓存项个数超过期望个数，超出限制时将尽可能进行置换。
     */
    SIT_NAT_cache(comm_dev *device, size_t expect_cache_size) {
        dev = device;
        expect_size = expect_cache_size;
        cur_size = 0;
    }

    /* 
     * 获取lpa缓存块
     * lpa未被缓存时，将同步从SSD读取内容并缓存
     * 若缓存个数超过预期大小，则尝试进行释放 
     */
    SIT_NAT_cache_entry *get_cache_entry(uint32_t lpa);

    void pin(uint32_t lpa) {
        cache_manager.pin(lpa);
    }

    void unpin(uint32_t lpa) {
        cache_manager.unpin(lpa);
    }

private:
    generic_cache_manager<uint32_t, SIT_NAT_cache_entry> cache_manager;
    size_t expect_size, cur_size;
    comm_dev *dev;

    /* 若缓存数量超过阈值，则尝试置换。*/
    void do_replace();
};

}  // namespace hscfs