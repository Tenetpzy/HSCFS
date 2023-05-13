#include "cache/page_cache.hh"
#include "utils/lock_guards.hh"
#include "utils/hscfs_log.h"
#include "fs/fs.h"
#include <system_error>

namespace hscfs {

page_cache::page_cache(size_t expect_size)
{
    int ret = spin_init(&cache_lock);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "page cache: init cache spin failed.");
    /* 任意对锁初始化失败的异常均为panic，上层应该捕获异常并终止程序，故此处不再释放成功初始化的锁 */
    ret = spin_init(&dirty_pages_lock);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "page cache: init dirty page set spin failed.");

    this->expect_size = expect_size;
    cur_size = 0;
}

page_cache::~page_cache()
{
    // page cache析构内不会并发访问，不加锁
    if (!dirty_pages.empty())
        HSCFS_LOG(HSCFS_LOG_WARNING, "page cache still has dirty page while destructed.");
}

page_entry_handle page_cache::get(uint32_t blkoff)
{
    spin_lock_guard lg(cache_lock);
    page_entry *p_entry = cache_manager.get(blkoff);
    
    /* 缓存项不存在，则需要新建一个 */
    if (p_entry == nullptr)
    {
        /*
         * 如果当前缓存数量已达到阈值，则尝试置换一个，直接把置换出的page_entry资源移动给新缓存项
         * 如果无法置换，再新建
         * 防止多次分配和释放4KB block
         */
        if (cur_size >= expect_size)
        {
            auto victim = cache_manager.replace_one();
            if (victim != nullptr)
            {
                assert(victim->ref_count == 0 && victim->is_dirty.load() == false);
                HSCFS_LOG(HSCFS_LOG_INFO, "replace page cache entry, blkoff = %u", victim->blkoff);
                victim->blkoff = blkoff;
                victim->content_state = page_state::invalid;

                p_entry = victim.get();
                cache_manager.add(blkoff, victim);
                add_refcount(p_entry);

                do_replace();  // 尝试将先前无法淘汰的缓存项淘汰掉，否则page_cache中缓存项数量单调不减
            }
        }

        /* 到此处，说明要么缓存容量充足，要么找不到能置换的缓存项，则直接增加一项 */
        if (p_entry == nullptr)
        {
            auto new_entry = std::make_unique<page_entry>(blkoff);
            p_entry = new_entry.get();
            cache_manager.add(blkoff, new_entry);
            add_refcount(p_entry);
            ++cur_size;
        }
    }

    /* 缓存项存在，增加引用计数即可 */
    else
        add_refcount(p_entry);

    return page_entry_handle(p_entry, this);
}

void page_cache::truncate(uint32_t max_blkoff)
{
    /* 由于调用者已经加了file_op_lock，内部不用加任何锁了 */
    auto start_itr = dirty_pages.upper_bound(max_blkoff);
    for (auto itr = start_itr; itr != dirty_pages.end(); ++itr)
    {
        /* 将范围外的所有page的dirty标记清除，标记为invalid */
        auto &handle = itr->second;
        handle->is_dirty = false;
        handle->content_state = page_state::invalid;
    }

    /* 从dirty pages集合中移除范围外的所有page */
    dirty_pages.erase(start_itr, dirty_pages.end());
}

/* 调用者需要加cache_lock，除非能够保证调用时ref_count不会为0 */
void page_cache::add_refcount(page_entry *entry)
{
    auto origin = entry->ref_count.fetch_add(1);
    if (origin == 0)
    {   
        /* 
         * 先前引用计数为0，不可能是dirty状态 
         * 此时ref_count由0增至1，且加了cache_lock，assert访问entry是安全的(此时只可能在此处访问)
         */
        assert(entry->is_dirty.load() == false);

        /*
         * 可能出现ref_count减到0，但减少ref_count的线程还没来得及unpin的情况(见sub_refcount)
         * 由于generic_cache_manager使用的lru_replacer允许重复调用pin，所以不会造成影响
         */
        cache_manager.pin(entry->blkoff);
    }
}

/* page_entry_handle析构时调用 */
void page_cache::sub_refcount(page_entry *entry)
{
    auto origin = entry->ref_count.fetch_sub(1);

    /* 由调用者线程将引用计数减为0，此时应当考虑unpin */
    if (origin == 1)
    {
        /*
         * 加cache_lock，再次检查引用计数
         * 成功获取锁后，如果ref_count仍为0，则不可能有其它线程能够修改ref_count，
         * 因为ref_count从0增加到1，一定是通过调用page_cache.get方法，而该方法在加ref_count前需要加cache_lock
         * 此时将其unpin，然后解锁
         */
        spin_lock_guard lg(cache_lock);
        if (entry->ref_count.load() == 0)
        {
            /* 
             * 若引用计数减为0，不可能是dirty状态
             * 此时ref_count由1减至0，且加了cache_lock，访问entry是安全的
             */
            assert(entry->is_dirty.load() == false);
            cache_manager.unpin(entry->blkoff);
        }

        /* 如果ref_count此时不为0，说明加锁前有其它线程再次通过get获取引用计数，所以放弃unpin */
    }
}

/* 调用者需要获取cache_lock */
void page_cache::do_replace()
{
    if (cur_size > expect_size)
    {
        while (true)
        {
            auto p = cache_manager.replace_one();
            if (p != nullptr)
            {
                assert(p->ref_count == 0 && p->is_dirty.load() == false);
                --cur_size;
                HSCFS_LOG(HSCFS_LOG_INFO, "replace page cache entry, blkoff = %u", p->blkoff);
            }
            if (p == nullptr || cur_size <= expect_size)
                break;
        }
    }
}

void page_cache::add_to_dirty_pages(page_entry_handle &page)
{
    spin_lock_guard lg(dirty_pages_lock);
    assert(page->ref_count.load() >= 1);
    dirty_pages.emplace(page->blkoff, page);
    add_refcount(page.entry);
}

page_entry_handle::page_entry_handle(const page_entry_handle &o)
    : page_entry_handle(o.entry, o.cache)
{
    /* handle拷贝，能保证ref_count不为0 */
    do_addref();
}

page_entry_handle &page_entry_handle::operator=(const page_entry_handle &o)
{
    if (this != &o)
    {
        do_subref();
        entry = o.entry;
        cache = o.cache;
        do_addref();
    }
    return *this;
}

page_entry_handle &page_entry_handle::operator=(page_entry_handle &&o)
{
    if (this != &o)
    {
        do_subref();
        cache = o.cache;
        entry = o.entry;
        o.entry = nullptr;
    }
    return *this;
}

page_entry_handle::~page_entry_handle()
{
    if (entry != nullptr)
    {
        try
        {
            cache->sub_refcount(entry);
        }
        catch(const std::exception& e)
        {
            HSCFS_LOG(HSCFS_LOG_WARNING, "exception during sub_refcount of page cache entry: "
                "%s", e.what());
        }
    }
}

void page_entry_handle::mark_dirty()
{
    /* 若返回true，说明是由本线程将dirty置位，因此本线程负责将其加入cache的dirty page set */
    if (entry->mark_dirty())
        cache->add_to_dirty_pages(*this);
}

void page_entry_handle::do_addref()
{
    if (entry != nullptr)
        cache->add_refcount(entry);
}

void page_entry_handle::do_subref()
{
    if (entry != nullptr)
        cache->sub_refcount(entry);
}

page_entry::page_entry(uint32_t blkoff)
{
    this->blkoff = blkoff;
    origin_lpa = commit_lpa = INVALID_LPA;
    content_state = page_state::invalid;
    ref_count.store(0);
    is_dirty.store(false);    
}

page_entry::~page_entry()
{
    if (ref_count != 0)
        HSCFS_LOG(HSCFS_LOG_WARNING, "page cache entry(blkoff = %u): "
            "refcount = %u while destructed.", blkoff, ref_count.load());
    if (is_dirty != 0)
        HSCFS_LOG(HSCFS_LOG_WARNING, "page cache entry(blkoff = %u): "
            "still dirty while destructed.", blkoff);
}

} // namespace hscfs