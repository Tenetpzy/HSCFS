#pragma once

#include <unordered_map>
#include <list>
#include <utility>
#include <memory>
#include <system_error>
#include <cassert>

#include "utils/hscfs_multithread.h"
#include "utils/spin_lock_guard.hh"

namespace hscfs {

/*
 * 缓存索引管理器
 * 缓存项类型为entry_t，要求缓存项具有唯一标识key(key_t类型)
 * 当缓存项添加到索引后，由索引获得其所有权
 */
template <typename key_t, typename entry_t>
class cache_hash_index
{
public:
    void add(const key_t &key, std::unique_ptr<entry_t> &p_entry)
    {
        assert(index.count(key) == 0);
        index.emplace(key, p_entry.release());
    }

    void add(const key_t &key, std::unique_ptr<entry_t> &&p_entry)
    {
        assert(index.count(key) == 0);
        index.emplace(key, p_entry);
    }

    std::unique_ptr<entry_t> remove(const key_t &key)
    {
        assert(index.count(key) == 1);
        std::unique_ptr<entry_t> ret = std::move(index[key]);
        index.erase(key);
        return ret;
    }

    entry_t *get(const key_t &key)
    {
        if (index.count(key) == 0)
            return nullptr;
        return index[key].get();
    }

private:
    std::unordered_map<key_t, std::unique_ptr<entry_t>> index;
};

/*
 * 缓存lru置换器
 * 使用缓存项的key代表缓存项
 */
template <typename key_t>
class lru_replacer
{
public:
    /* 
     * 将key加入置换器
     * 默认key可进行置换
     */
    void add(const key_t &key)
    {
        assert(key_states.count(key) == 0);
        key_states[key].first = false;
        add_to_list_tail(lru_list, key);
    }

    /* 返回可以被置换的缓存项数目 */
    size_t get_num_can_replace()
    {
        return lru_list.size();
    }

    /* 进行置换，返回应当被置换的key */
    key_t pop_replaced()
    {
        assert(lru_list.empty() == false);
        key_t key = lru_list.front();
        assert(key_states.at(key).first == false);
        assert(key_states.at(key).second == lru_list.begin());
        lru_list.pop_front();
        key_states.erase(key);
        return key;
    }

    /* 
     * 锁住key，让其不能被置换
     * 如果已经被锁住，什么也不做
     */
    void pin(const key_t &key)
    {
        auto &element = key_states.at(key);
        if (element.first == true)
            return;
        element.first = true;
        lru_list.erase(element.second);
        add_to_list_tail(pinned_list, key);
    }

    /*
     * 解锁key，使其可以被置换
     * unpin被视为一次访问，会将其放置到lru链表尾
     * 如果没有被锁住，什么都不做
     */
    void unpin(const key_t &key)
    {
        auto &element = key_states.at(key);
        if (element.first == false)
            return;
        element.first = false;
        pinned_list.erase(element.second);
        add_to_list_tail(lru_list, key);
    }

    /* 告知replacer对key进行了一次访问 */
    void access(const key_t &key)
    {
        assert(key_states.count(key) == 1);
        auto &element = key_states[key];

        // 访问被pin住的缓存，不用管
        if (element.first == true)
            return;
        
        // 否则移到lru链表尾
        lru_list.erase(element.second);
        add_to_list_tail(lru_list, key);
    }

private:
    /* 
     * lru链表lru_list，不能置换的链表pinned_list
     * lru链表中，首元素为最近最少访问的元素
     */
    std::list<key_t> lru_list, pinned_list;

    using list_iterator_t = typename std::list<key_t>::iterator;

    /* 
     * map的value: first为当前该缓存项是否被pin住(true则被pin住)，second为list的iterator
     * 同一时刻，根据缓存项是否被pin住，该iterator指向的元素位于lru_list(unpin)或pinned_list(pin)二者之一
     */ 
    std::unordered_map<key_t, std::pair<bool, list_iterator_t>> key_states;

    void add_to_list_tail(std::list<key_t> &ls, const key_t &key)
    {
        ls.emplace_back(key);
        key_states[key].second = --ls.end();
    }
};

/*
 * 通用缓存管理器
 * 要求：
 * 
 * 缓存项类型为entry_t，缓存项的键的类型为key_t
 * 
 * 缓存索引管理器的类型为 index_t<key_t, entry_t>，能够调用：
 * void add(const key_t &key, std::unique_ptr<entry_t> &p_entry);
 * std::unique_ptr<entry_t> remove(const key_t &key);
 * entry_t *get(const key_t &key);
 * 
 * 缓存置换器的类型为 replacer_t<key_t>，能够调用：
 * void add(const key_t &key);
 * size_t get_num_can_replace();
 * key_t pop_replaced();
 * void pin(const key_t &key);
 * void unpin(const key_t &key);
 * void access(const key_t &key);
 * 
 */
template <typename key_t, typename entry_t, 
    template <typename, typename> class index_t = cache_hash_index, 
    template <typename> class replacer_t = lru_replacer>
class generic_cache_manager
{
public:
    /* 增加缓存项p_entry，并把该缓存项的所有权交给cache_manager */
    void add(const key_t &key, std::unique_ptr<entry_t> &p_entry)
    {
        index.add(key, p_entry);
        replacer.add(key);
    }

    void add(const key_t &key, std::unique_ptr<entry_t> &&p_entry)
    {
        index.add(key, std::move(p_entry));
        replacer.add(key);
    }

    /* 
     * 获取键为key的缓存项，若不存在返回nullptr
     * 若is_access为true，则视为对key的一次访问
     * 调用者不拥有该缓存项指针的所有权，不能释放 
     */
    entry_t *get(const key_t &key, bool is_access = true)
    {
        entry_t *ret = index.get(key);
        if (ret != nullptr && is_access)
            replacer.access(key);
        return ret;
    }

    /* pin住缓存项，标识其不能被置换 */
    void pin(const key_t &key)
    {
        replacer.pin(key);
    }

    /* unpin缓存项，使其可以被置换 */
    void unpin(const key_t &key)
    {
        replacer.unpin(key);
    }

    /*
     * 置换一个缓存项，将其从cache_manager中移除并获取其所有权
     * 若没有可置换的缓存项，返回nullptr
     */
    std::unique_ptr<entry_t> replace_one()
    {
        if (replacer.get_num_can_replace() == 0)
            return nullptr;
        key_t key = replacer.pop_replaced();
        return index.remove(key);
    }

private:
    index_t<key_t, entry_t> index;  // 缓存索引
    replacer_t<key_t> replacer;  // 置换器
};


/* 
 * 线程安全的通用缓存管理器
 * API语义与generic_cache_manager相同
 */
template <typename key_t, typename entry_t, 
    template <typename, typename> class index_t = cache_hash_index, 
    template <typename> class replacer_t = lru_replacer>
class generic_cache_manager_safe
{
public:
    generic_cache_manager_safe() 
    {
        int ret = spin_init(&lock);
        if (ret != 0)
            throw std::system_error(std::error_code(ret, std::generic_category()), 
                "generic cache manager safe: init spin lock failed.");
    }

    ~generic_cache_manager_safe()
    {
        int ret = spin_destroy(&lock);
        if (ret != 0)
            HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "generic cache manager safe: destroy spin lock failed.");
    }

    void add(const key_t &key, std::unique_ptr<entry_t> &p_entry)
    {
        spin_lock_guard lg(&lock);
        cache_manager.add(key, p_entry);
    }

    void add(const key_t &key, std::unique_ptr<entry_t> &&p_entry)
    {
        spin_lock_guard lg(&lock);
        cache_manager.add(key, std::move(p_entry));
    }

    entry_t *get(const key_t &key, bool is_access = true)
    {
        spin_lock_guard lg(&lock);
        return cache_manager.get(key, is_access);
    }

    void pin(const key_t &key)
    {
        spin_lock_guard lg(&lock);
        cache_manager.pin(key);
    }

    void unpin(const key_t &key)
    {
        spin_lock_guard lg(&lock);
        cache_manager.unpin(key);
    }

    std::unique_ptr<entry_t> replace_one()
    {
        spin_lock_guard lg(&lock);
        return cache_manager.replace_one();
    }

private:
    generic_cache_manager<key_t, entry_t, index_t, replacer_t> cache_manager;
    spinlock_t lock;
};

}