#include <unordered_map>
#include <iostream>
#include <ctime>
#include <string>

#include "fmt/ostream.h"
#include "communication/vendor_cmds.h"
#include "journal/journal_type.h"
#include "fs/path_utils.hh"
#include "fs/fs.h"
#include "utils/hscfs_log.h"

namespace hscfs {

/* 日志打印 */
/***********************************************************/

class journal_entry_printer
{
public:
    journal_entry_printer(const char *start)
    {
        start_addr = start;
    }
    virtual ~journal_entry_printer() = default;

    virtual void print() = 0;

protected:
    const char *start_addr;

    static void println_header(const meta_journal_entry *p_header)
    {
        static std::unordered_map<uint8_t, std::string> str_of_type = {
            {JOURNAL_TYPE_NATS, "NAT"}, {JOURNAL_TYPE_SITS, "SIT"}, 
            {JOURNAL_TYPE_SUPER_BLOCK, "SUPER"}, {JOURNAL_TYPE_NOP, "NOP"}, 
            {JOURNAL_TYPE_END, "END"}  
        };
        std::string type_str;
        if (str_of_type.count(p_header->type))
            type_str = str_of_type[p_header->type];
        else
            type_str = "UNKNOWN";
        
        fmt::println(std::cerr, "type: {}, len: {}", type_str, p_header->len);
    }

    static size_t calculate_entry_num(size_t journal_len, size_t entry_len)
    {
        auto err = [journal_len]() {
            fmt::println(std::cerr, "invalid journal len: {}", journal_len);
            abort();
        };

        if (journal_len < sizeof(meta_journal_entry) + entry_len)
            err();
        size_t journal_entry_len = journal_len - sizeof(meta_journal_entry);
        if (journal_entry_len % entry_len)
            err();
        return journal_entry_len / entry_len;
    }
};

class super_entry_printer: public journal_entry_printer
{
public:
    super_entry_printer(const char *start): journal_entry_printer(start) {}
    void print() override
    {
        auto p_header = reinterpret_cast<const meta_journal_entry*>(start_addr);
        println_header(p_header);

        size_t num = calculate_entry_num(p_header->len, super_entry_len);
        fmt::println(std::cerr, "total entry num: {}", num);
        auto p_entry = reinterpret_cast<const super_block_journal_entry*>(start_addr + sizeof(meta_journal_entry));
        for (size_t i = 1; i <= num; ++i, ++p_entry)
            fmt::println(std::cerr, "entry {}: off = {}", i, p_entry->Off);
    }

private:
    const uint16_t super_entry_len = sizeof(super_block_journal_entry);
};

class NAT_entry_printer: public journal_entry_printer
{
public:
    NAT_entry_printer(const char *start): journal_entry_printer(start) {}
    void print() override
    {
        auto p_header = reinterpret_cast<const meta_journal_entry*>(start_addr);
        println_header(p_header);

        size_t num = calculate_entry_num(p_header->len, NAT_entry_len);
        fmt::println(std::cerr, "total entry num: {}", num);
        auto p_entry = reinterpret_cast<const NAT_journal_entry*>(start_addr + sizeof(meta_journal_entry));
        for (size_t i = 1; i <= num; ++i, ++p_entry)
            fmt::println(std::cerr, "entry {}: nid = {}", i, p_entry->nid);
    }

private:
    const uint16_t NAT_entry_len = sizeof(NAT_journal_entry);
};

class SIT_entry_printer: public journal_entry_printer
{
public:
    SIT_entry_printer(const char *start): journal_entry_printer(start) {}
    void print() override
    {
        auto p_header = reinterpret_cast<const meta_journal_entry*>(start_addr);
        println_header(p_header);

        size_t num = calculate_entry_num(p_header->len, SIT_entry_len);
        fmt::println(std::cerr, "total entry num: {}", num);
        auto p_entry = reinterpret_cast<const SIT_journal_entry*>(start_addr + sizeof(meta_journal_entry));
        for (size_t i = 1; i <= num; ++i, ++p_entry)
            fmt::println(std::cerr, "entry {}: segid = {}", i, p_entry->segID);
    }

private:
    const uint16_t SIT_entry_len = sizeof(SIT_journal_entry);
};

class NOP_END_entry_printer: public journal_entry_printer
{
public:
    NOP_END_entry_printer(const char *start): journal_entry_printer(start) {}
    void print() override
    {
        auto p_header = reinterpret_cast<const meta_journal_entry*>(start_addr);
        println_header(p_header);
    }
};

class journal_printer_factory
{
public:
    static std::unique_ptr<journal_entry_printer> generate(uint8_t type, const char *start)
    {
        std::unique_ptr<journal_entry_printer> ret;
        switch (type)
        {
        case JOURNAL_TYPE_SUPER_BLOCK:
            ret.reset(new super_entry_printer(start));
            break;
        
        case JOURNAL_TYPE_NATS:
            ret.reset(new NAT_entry_printer(start));
            break;
        
        case JOURNAL_TYPE_SITS:
            ret.reset(new SIT_entry_printer(start));
            break;
        
        case JOURNAL_TYPE_NOP:
        case JOURNAL_TYPE_END:
            ret.reset(new NOP_END_entry_printer(start));
            break;

        default:
            throw std::logic_error("journal printer factory: invalid type.");
            break;
        }
        return ret;
    };
};

void print_journal_block(const char *start)
{
    const char *p_journal = start;
    while (true)
    {
        auto cur_journal_entry = reinterpret_cast<const meta_journal_entry*>(p_journal);
        auto cur_len = cur_journal_entry->len;

        auto journal_printer = journal_printer_factory::generate(cur_journal_entry->type, p_journal);
        journal_printer->print();
        std::cerr << std::endl;
        if (cur_journal_entry->type == JOURNAL_TYPE_END)
            break;
        if (cur_journal_entry->type == JOURNAL_TYPE_NOP)
        {
            if (p_journal + cur_len - 4096 != start)
            {
                fmt::print(std::cerr, "warning! Invalid NOP entry pos or len. \
                    NOP at offset {}, len = {}", p_journal - start, cur_len);
            }
            break;
        }
        p_journal += cur_len;
        if (p_journal - start == 4096)
            break;
    }
}

/***********************************************************/

/* path lookup打印 */
/***********************************************************/

void print_path_lookup_task(path_lookup_task *task)
{
    char *p = reinterpret_cast<char*>(task) + sizeof(path_lookup_task);
    std::string path(p, task->pathlen);
    HSCFS_LOG(HSCFS_LOG_INFO, "send path lookup task:\n");
    fmt::println(std::cerr, 
        "start inode: {}\n"
        "depth: {}\n"
        "pathlen: {}\n"
        "path: {}\n",
        task->start_ino, task->depth, task->pathlen, path
    );
}

/***********************************************************/

/* filemapping search 打印 */
/***********************************************************/

void print_filemapping_search_task(filemapping_search_task *task)
{
    HSCFS_LOG(HSCFS_LOG_INFO, "send filemapping search task:\n");
    fmt::println(std::cerr,
        "inode: {}\n"
        "start nid: {}\n"
        "file block offset: {}\n"
        "is return all level: {}\n",
        task->ino, task->nid_to_start, task->file_blk_offset, static_cast<bool>(task->return_all_Level)
    );
}

void print_node_footer(hscfs_node *node);

void print_filemapping_search_result(hscfs_node *node, uint32_t level_num)
{
    HSCFS_LOG(HSCFS_LOG_INFO, "result of SSD filemapping search: level_num = %u", level_num);
    for (uint32_t i = 0; i < level_num; ++i, ++node)
        print_node_footer(node);
    fmt::print(std::cerr, "\n");
}

/************************************************************/

/* node数据打印 */
/***********************************************************/

void print_inode_meta(uint32_t ino, hscfs_inode *inode)
{
    HSCFS_LOG(HSCFS_LOG_INFO, "print inode metadata: \n");

    char atime_buf[128], mtime_buf[128];
    time_t atime = inode->i_atime;
    time_t mtime = inode->i_mtime;
    strftime(atime_buf, sizeof(atime_buf), "%D %T", gmtime(&atime));
    strftime(mtime_buf, sizeof(mtime_buf), "%D %T", gmtime(&mtime));

    fmt::println(std::cerr, 
        "inode: {}\n"
        "hard link number: {}\n"
        "size: {} bytes\n"
        "access time: {}\n"
        "modify time: {}\n",
        ino, inode->i_nlink, inode->i_size, atime_buf, mtime_buf
    );
}

void print_node_footer(hscfs_node *node)
{
    node_footer *footer = &node->footer;
    fmt::println(std::cerr, "nid = {}, ino = {}, offset = {}", 
        footer->nid, footer->ino, footer->offset);
}

/***********************************************************/

}  // namespace hscfs