#pragma once

#include <cstdint>
#include <memory>

#include "cache/node_block_cache.hh"
#include "cache/dentry_cache.hh"

namespace hscfs {

class journal_container;

/* 一个事务的缓存淘汰保护信息 */
class transaction_replace_protect_record
{
public:
    transaction_replace_protect_record(uint32_t tx_id, std::list<node_block_cache_entry_handle> &&dirty_nodes_,
        std::vector<dentry_handle> &&dirty_dentrys_, std::unique_ptr<journal_container> &tx_journal, 
        std::vector<uint32_t> &&uncommit_node_segs_, std::vector<uint32_t> &&uncommit_data_segs_);
    
    

private:
    uint32_t tx_id;

    /* 此处相当于对node缓存项和dentry缓存项的主机测版本号+1，不让它们被淘汰(版本号在ref_count中一同维护) */
    std::list<node_block_cache_entry_handle> dirty_nodes;
    std::vector<dentry_handle> dirty_dentrys;

    /* 该事务积累的日志。事务日志在SSD应用后，根据日志中的内容增加SIT和NAT表的SSD侧版本号 */
    std::unique_ptr<journal_container> tx_journal;

    /* 该事务写满的node和data segments。SSD应用完日志后，将它们加入到系统的node和data segments中 */
    std::vector<uint32_t> uncommit_node_segs, uncommit_data_segs;
};

};