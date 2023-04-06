#pragma once

#include "cache/cache_manager.hh"
#include "cache/block_buffer.hh"

struct comm_dev;
struct hscfs_sit_block;
struct hscfs_nat_block;

namespace hscfs {

class SIT_NAT_cache_entry
{
public:
    SIT_NAT_cache_entry(uint32_t lpa) {
        lpa_ = lpa;
        ref_count = 0;
    }

    ~SIT_NAT_cache_entry();

    uint32_t lpa_;
    uint32_t ref_count;
    block_buffer cache;
};

class SIT_NAT_cache;

/*
 * 由SIT_NAT_cache返回的缓存项控制句柄
 * 句柄的作用：句柄代表缓存项的读写引用计数。句柄存在时，缓存项的引用计数+1，句柄析构时，缓存项的引用计数-1
 */
class SIT_NAT_cache_entry_handle
{
public:
    SIT_NAT_cache_entry_handle() noexcept
    {
        entry_ = nullptr;
    }

    /* 此构造函数由SIT_NAT_cache使用，构造前增加了引用计数 */
    SIT_NAT_cache_entry_handle(SIT_NAT_cache_entry *entry, std::shared_ptr<SIT_NAT_cache> &&cache) 
    {
        entry_ = entry;
        cache_ = cache;
    }

    SIT_NAT_cache_entry_handle(const SIT_NAT_cache_entry_handle &o);

    SIT_NAT_cache_entry_handle(SIT_NAT_cache_entry_handle &&o) noexcept
    {
        entry_ = o.entry_;
        cache_ = std::move(o.cache_);
        o.entry_ = nullptr;
    }

    SIT_NAT_cache_entry_handle& operator=(const SIT_NAT_cache_entry_handle &o);
    SIT_NAT_cache_entry_handle& operator=(SIT_NAT_cache_entry_handle &&o) noexcept;

    ~SIT_NAT_cache_entry_handle();

    /* 调用者应保证，在调用以下方法时，this是有效的（未被移动） */
    void add_host_version();
    void add_SSD_version();
    
    hscfs_sit_block *get_sit_block_ptr() noexcept 
    {
        return reinterpret_cast<hscfs_sit_block*>(entry_->cache.get_ptr());
    }

    hscfs_nat_block *get_nat_block_ptr() noexcept 
    {
        return reinterpret_cast<hscfs_nat_block*>(entry_->cache.get_ptr());
    }

private:
    SIT_NAT_cache_entry *entry_;

    /* 
     * 活跃segment对应的缓存项可能会常驻内存，为了避免SIT_NAT_cache对象先析构，
     * 导致此对象析构时引用悬挂指针，使用shared_ptr 
     */
    std::shared_ptr<SIT_NAT_cache> cache_;

    /* 封装entry_为nullptr的判断 */
    void do_addref();
    void do_subref();
};

/* 
 * SIT、NAT缓存控制器
 * 以lpa(uint32_t)作为key，SIT_NAT_cache_entry作为缓存项
 * 此对象必须由shared_ptr管理
 */
class SIT_NAT_cache: public std::enable_shared_from_this<SIT_NAT_cache>
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
    SIT_NAT_cache(comm_dev *device, size_t expect_cache_size) 
    {
        dev = device;
        expect_size = expect_cache_size;
        cur_size = 0;
    }

    /* 
     * 获取lpa对应缓存项句柄
     * lpa未被缓存时，将同步从SSD读取内容并缓存
     * 会对缓存项调用add_refcount
     * 若缓存个数超过预期大小，则尝试进行释放 
     */
    SIT_NAT_cache_entry_handle get(uint32_t lpa) 
    {
        SIT_NAT_cache_entry *p = get_cache_entry_inner(lpa);
        add_refcount(p);
        do_replace();
        return SIT_NAT_cache_entry_handle(p, shared_from_this());
    }

    /* 
     * 在处理已完成事务的日志时，可直接通过日志里的lpa增加缓存项版本号，
     * 不必使用get_cache_entry获取handle再增加其版本号
     */
    void add_SSD_version(uint32_t lpa) 
    {
        SIT_NAT_cache_entry *p = get_cache_entry_inner(lpa, false);
        sub_refcount(p);
    }

private:
    generic_cache_manager<uint32_t, SIT_NAT_cache_entry> cache_manager;
    size_t expect_size, cur_size;
    comm_dev *dev;

private:
    /*
     * 释放对entry的引用。
     * 会对缓存项调用sub_refcount
     */
    void put_cache_entry(SIT_NAT_cache_entry *entry) 
    {
        sub_refcount(entry);
    }

    /*
     * 将entry的引用计数+1
     * 如果引用计数之前为0，则pin住缓存项
     */
    void add_refcount(SIT_NAT_cache_entry *entry) 
    {
        ++entry->ref_count;
        if (entry->ref_count == 1)
            cache_manager.pin(entry->lpa_);
    }

    /*
     * 将entry的引用计数-1
     * 如果引用计数减至0，则unpin缓存项
     */
    void sub_refcount(SIT_NAT_cache_entry *entry) 
    {
        --entry->ref_count;
        if (entry->ref_count == 0)
        {
            cache_manager.unpin(entry->lpa_);
            do_replace();
        }
    }

    /* entry的主机侧/SSD版本号管理 */
    void add_host_version(SIT_NAT_cache_entry *entry) 
    {
        add_refcount(entry);
    }

    void add_SSD_version(SIT_NAT_cache_entry *entry) 
    {
        sub_refcount(entry);
    }

    /* 
     * 通过lpa在cache_manager中查找缓存项，找不到则从SSD读取。
     * 不增加缓存项的refcount
     */
    SIT_NAT_cache_entry *get_cache_entry_inner(uint32_t lpa, bool is_access = true)
    {
        SIT_NAT_cache_entry *p = cache_manager.get(lpa, is_access);

        // 如果缓存不命中，则从SSD读取。
        if (p == nullptr)
        {
            auto tmp = std::make_unique<SIT_NAT_cache_entry>(lpa);
            p = tmp.get();
            read_lpa(p);
            ++cur_size;
            cache_manager.add(lpa, tmp);
        }

        return p;
    }

    /* 若缓存数量超过阈值，则尝试置换。*/
    void do_replace();

    void read_lpa(SIT_NAT_cache_entry *entry);

    friend class SIT_NAT_cache_entry_handle;
};

}  // namespace hscfs