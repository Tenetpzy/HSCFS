#include "journal/journal_container.hh"
#include "journal/journal_process_env.hh"
#include "communication/comm_api.h"
#include "fs/block_buffer.hh"
#include "fmt/ostream.h"
#include "gtest/gtest.h"

#include <vector>
#include <stdexcept>
#include <cstring>
#include <string>
#include <iostream>
#include <unordered_map>
#include <memory>

struct journal_area_mock
{
    std::vector<hscfs_block_buffer> flash;
    uint64_t start_lpa, end_lpa, head_lpa, tail_lpa;
} journal_area;

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
        
        fmt::println(std::cout, "type: {}, len: {}", type_str, p_header->len);
    }

    static ssize_t calculate_entry_num(size_t journal_len, size_t entry_len)
    {
        size_t journal_entry_len = journal_len - sizeof(meta_journal_entry);
        if (journal_entry_len % entry_len)
            return -1;
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

        ssize_t num = calculate_entry_num(p_header->len, super_entry_len);
        if (num < 0)
        {
            std::cout << "invalid super entry len\n";
            throw std::logic_error("invalid super entry len.");
        }

        fmt::println(std::cout, "total entry num: {}", num);
        auto p_entry = reinterpret_cast<const super_block_journal_entry*>(start_addr + p_header->len);
        for (ssize_t i = 1; i <= num; ++i, ++p_entry)
            fmt::println(std::cout, "entry {}: off = {}", i, p_entry->Off);
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

        ssize_t num = calculate_entry_num(p_header->len, NAT_entry_len);
        if (num < 0)
        {
            std::cout << "invalid NAT entry len\n";
            throw std::logic_error("invalid NAT entry len.");
        }

        fmt::println(std::cout, "total entry num: {}", num);
        auto p_entry = reinterpret_cast<const NAT_journal_entry*>(start_addr + p_header->len);
        for (ssize_t i = 1; i <= num; ++i, ++p_entry)
            fmt::println(std::cout, "entry {}: nid = {}", i, p_entry->nid);
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

        ssize_t num = calculate_entry_num(p_header->len, SIT_entry_len);
        if (num < 0)
        {
            std::cout << "invalid NAT entry len\n";
            throw std::logic_error("invalid NAT entry len.");
        }

        fmt::println(std::cout, "total entry num: {}", num);
        auto p_entry = reinterpret_cast<const SIT_journal_entry*>(start_addr + p_header->len);
        for (ssize_t i = 1; i <= num; ++i, ++p_entry)
            fmt::println(std::cout, "entry {}: segid = {}", i, p_entry->segID);
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

int comm_submit_async_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count,
    comm_async_cb_func cb_func, void *cb_arg, comm_io_direction dir)
{
    if ((lba & 7) != 0)
        std::cerr << "read write mock: unaligned lba.\n";
    for (size_t i = 0; i < lba_count; ++i)
    {
        uint64_t tar_lba = lba + i;
        uint64_t tar_lpa = tar_lba >> 3;
        uint64_t lpa_off = (tar_lba & 3) * 512;
        char *p1 = journal_area.flash[tar_lpa].get_ptr() + lpa_off;
        char *p2 = static_cast<char*>(buffer) + i * 512;
        if (dir == COMM_IO_READ)
            std::memcpy(p2, p1, 512);
        else
            std::memcpy(p1, p2, 512);
    }
    cb_func(COMM_CMD_SUCCESS, cb_arg);
    return 0;
}

int comm_submit_sync_update_metajournal_tail_request(comm_dev *dev, uint64_t origin_lpa, uint32_t write_block_num)
{
    if (origin_lpa != journal_area.tail_lpa)
        throw std::logic_error("origin lpa does not equal to journal tail lpa.");
    
    // write_block_num check
    uint64_t num_can_write;
    uint64_t total_lpa_num = journal_area.end_lpa - journal_area.start_lpa;
    if (journal_area.tail_lpa >= journal_area.head_lpa)
        num_can_write = total_lpa_num - (journal_area.tail_lpa - journal_area.head_lpa) - 1;
    else
        num_can_write = journal_area.head_lpa - journal_area.tail_lpa - 1;
    if (write_block_num > num_can_write)
    {
        fmt::println(std::cerr, "write block num = {}, num can write = {}", write_block_num, num_can_write);
        throw std::logic_error("write block num is larger than num can write.");
    }
    
    journal_area.tail_lpa += write_block_num;
    if (journal_area.tail_lpa >= journal_area.end_lpa)
        journal_area.tail_lpa = journal_area.tail_lpa - journal_area.end_lpa + journal_area.start_lpa;
    
    return 0;
}

/* 
 * mock获取头指针，将头指针推进upper(unapplied_lpa_num / 2)，并print出推进的日志内容
 */
int comm_submit_sync_get_metajournal_head_request(comm_dev *dev, uint64_t *head_lpa)
{
    auto get_unapplied_lpa_num = [](const journal_area_mock &journal)
    {
        if (journal.tail_lpa >= journal.start_lpa)
            return journal.tail_lpa - journal.head_lpa;
        else
            return journal.tail_lpa + journal.end_lpa - journal.start_lpa - journal.head_lpa; 
    };

    uint64_t unapplied = get_unapplied_lpa_num(journal_area);
    if (unapplied == 0)
    {
        *head_lpa = journal_area.head_lpa;
        return 0;
    }

    uint64_t new_head = journal_area.head_lpa + (unapplied + 1) / 2;
    if (new_head >= journal_area.end_lpa)
        new_head = new_head - journal_area.end_lpa + journal_area.start_lpa;
    
    uint64_t &cur_head = journal_area.head_lpa;
    while (cur_head != new_head)
    {
        if (cur_head == journal_area.end_lpa)
        {
            cur_head = journal_area.start_lpa;
            continue;
        }

        char *block_start_addr = journal_area.flash[cur_head].get_ptr();
        char *p_journal = block_start_addr;
        while (true)
        {
            meta_journal_entry *cur_journal_entry = reinterpret_cast<meta_journal_entry*>(p_journal);
            auto cur_len = cur_journal_entry->len;

            auto journal_printer = journal_printer_factory::generate(cur_journal_entry->type, p_journal);
            journal_printer->print();
            if (cur_journal_entry->type == JOURNAL_TYPE_END)
                break;
            if (cur_journal_entry->type == JOURNAL_TYPE_NOP)
            {
                if (p_journal + cur_len - 4096 != block_start_addr)
                {
                    fmt::print(std::cout, "warning! Invalid NOP entry pos or len. \
                        NOP at offset {}, len = {}", p_journal - block_start_addr, cur_len);
                }
                break;
            }
            
            p_journal += cur_len;
        }

        ++cur_head;
    }

    *head_lpa = cur_head;
    return 0;
}



int main(int argc, char **argv)
{
    
}