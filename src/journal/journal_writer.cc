#include <unordered_map>
#include <map>

#include "journal/journal_writer.hh"
#include "journal/journal_container.hh"
#include "journal/journal_type.h"
#include "communication/memory.h"
#include "communication/comm_api.h"
#include "utils/hscfs_exceptions.hh"
#include "utils/io_utils.hh"

enum class journal_output_state
{
    OK, NO_ENOUGH_BUFFER, REACH_END
};

// 日志条目的合并与输出接口
class journal_output_vector
{
public:

    /*
     * 生成日志项输出向量
     * 向量中的每个元素是一个日志条目
     * 
     * 此接口内完成：
     * 修改相同目标的日志条目合并，仅保留最后一个日志条目，反应该目标的最新值
     * 将日志条目在输出向量中按一定顺序排列，SSD处理时能一次性处理多个目标在同一page内的日志条目
     */
    virtual void generate_output_vector() = 0;

    /*
     * 上层即将开始输出
     * output_to_buffer对调用者是无状态的，此接口内将初始化output状态，从输出向量头部开始
     */
    virtual void prepare_output() = 0;

    /*
     * 将日志项输出到调用者提供的缓存区域[*p_start_addr, end_addr)
     * 尽可能多地在缓存区域中输出，除非已经输出完毕或缓存空间不足。
     * 
     * 返回值：
     * OK：成功完成了输出，此时*p_start_addr被置为输出区域的尾后地址
     * NO_ENOUGH_BUFFER：提供的缓存空间不足以输出一个[首部+日志条目]，*p_start_addr不变。
     * REACH_END：已经输出完毕，*p_start_addr不变。
     * 
     * 多次调用此接口，则本次调用将继续上一次调用已输出的日志条目之后，进行输出。
     * 
     * 如果缓存区域没有全部写入，此接口在尾部至少留下一个NOP日志项的空间，
     * 除非输入的缓存区域不足以放下NOP。（需由调用方，即journal_writer保证，提供的缓存区足够放下NOP）
     */
    virtual journal_output_state output_to_buffer(char **p_start_addr, char *end_addr) = 0;

    virtual ~journal_output_vector() = default;

protected:
    /*
     * 计算能够写入的日志条目个数的通用算法
     * buffer_size：目标缓存区大小(字节为单位)
     * entry_size：日志条目大小(字节为单位)
     * expected_write_num：期望写入的日志条目个数
     * 
     * 返回实际能写入的日志条目个数
     * 算法考虑日志项首部，且保证：若不是恰好把缓存写满，则在尾部留下NOP空间（除非输入的buffer_size本身无法写入NOP）
     */
    static uint64_t generic_calculate_writable_entry_num(uint64_t buffer_size, uint64_t entry_size, 
        uint64_t expected_write_num)
    {
        uint64_t ret;
        const uint64_t entry_header_len = sizeof(meta_journal_entry);
        uint64_t expected_write_len = entry_header_len + expected_write_num * entry_size;
        if (expected_write_len < buffer_size)
        {
            if (buffer_size - expected_write_len >= entry_header_len)
                ret = expected_write_num;
            else if (buffer_size > 2 * entry_header_len)
                ret = (buffer_size - 2 * entry_header_len) / entry_size;
            else
                ret = 0;
        }
        else if (expected_write_len == buffer_size)
            ret = expected_write_num;
        else
        {
            if (buffer_size < entry_header_len)
                ret = 0;
            else
                ret = generic_calculate_writable_entry_num(buffer_size, entry_size, 
                    (buffer_size - entry_header_len) / entry_size);
        }
        return ret;
    }
};

// 实现通用output_to_buffer方法
template <typename MapT, typename journalT>
class general_journal_output_vector: public journal_output_vector
{
public:
    general_journal_output_vector(const std::vector<journalT> &journal_entry, uint8_t journal_type): 
        journal(journal_entry), _journal_type(journal_type) { }

    void prepare_output() override
    {
        output_it = j_map.begin();
        rest_output_num = j_map.size();
    }

    journal_output_state output_to_buffer(char **p_start_addr, char *end_addr) override
    {
        if (rest_output_num == 0)
            return journal_output_state::REACH_END;
        
        size_t output_num = generic_calculate_writable_entry_num(end_addr - *p_start_addr, 
            sizeof(journalT), rest_output_num);
        if (output_num == 0)
            return journal_output_state::NO_ENOUGH_BUFFER;
        
        char *p = *p_start_addr;
        meta_journal_entry header;
        header.len = sizeof(meta_journal_entry) + output_num * sizeof(journalT);
        header.type = _journal_type;
        *reinterpret_cast<meta_journal_entry*>(p) = header;
        p += sizeof(meta_journal_entry);

        for (size_t i = 0; i < output_num; ++i, ++output_it, p += sizeof(journalT))
            *reinterpret_cast<journalT*>(p) = journal[output_it->second];
        
        rest_output_num -= output_num;
        *p_start_addr = p;
        return journal_output_state::OK;
    }

protected:
    const std::vector<journalT> &journal;
    uint8_t _journal_type;

    /* 
     * 使用map或unordered_map为journal去重并保留最新值，map还可以按journal某字段排序
     * key为特定日志条目的唯一标识
     * value为日志条目在journal数组的下标
     */
    MapT j_map;
    using map_iterator_t = typename MapT::iterator;
    map_iterator_t output_it;
    size_t rest_output_num;
};

// super block日志项输出实现
using super_journal_output_vector_impl = general_journal_output_vector<std::unordered_map<uint32_t, size_t>, 
    super_block_journal_entry>;
class super_journal_output_vector: public super_journal_output_vector_impl
{
public:
    super_journal_output_vector(const std::vector<super_block_journal_entry> &super_journal):
        super_journal_output_vector_impl(super_journal, JOURNAL_TYPE_SUPER_BLOCK) {}

    void generate_output_vector() override
    {
        j_map.clear();
        for (size_t i = 0; i < journal.size(); ++i)
            j_map[journal[i].Off] = i;
    }
};

// NAT日志项输出实现
using NAT_journal_output_vector_impl = general_journal_output_vector<std::map<uint32_t, size_t>, NAT_journal_entry>;
class NAT_journal_output_vector: public NAT_journal_output_vector_impl
{
public:
    NAT_journal_output_vector(const std::vector<NAT_journal_entry> &NAT_journal):
        NAT_journal_output_vector_impl(NAT_journal, JOURNAL_TYPE_NATS) {}

    void generate_output_vector() override
    {
        j_map.clear();
        for (size_t i = 0; i < journal.size(); ++i)
            j_map[journal[i].nid] = i;
    }
};

// SIT日志项输出实现
using SIT_journal_output_vector_impl = general_journal_output_vector<std::map<uint32_t, size_t>, SIT_journal_entry>;
class SIT_journal_output_vector: public SIT_journal_output_vector_impl
{
public:
    SIT_journal_output_vector(const std::vector<SIT_journal_entry> &SIT_journal):
        SIT_journal_output_vector_impl(SIT_journal, JOURNAL_TYPE_SITS) {}
    
    void generate_output_vector() override
    {
        j_map.clear();
        for (size_t i = 0; i < journal.size(); ++i)
            j_map[journal[i].segID] = i;
    }
};

std::vector<std::unique_ptr<journal_output_vector>> hscfs_journal_writer::journal_output_vec_generate() const
{
    std::vector<std::unique_ptr<journal_output_vector>> ret;
    auto &SIT_journal = cur_journal->get_SIT_journal();
    if (!SIT_journal.empty())
        ret.emplace_back(new SIT_journal_output_vector(SIT_journal));

    auto &NAT_journal = cur_journal->get_NAT_journal();
    if (!NAT_journal.empty())
        ret.emplace_back(new NAT_journal_output_vector(NAT_journal));

    auto &super_journal = cur_journal->get_super_block_journal();
    if (!super_journal.empty())
        ret.emplace_back(new super_journal_output_vector(super_journal));
    
    return ret;
}

char *hscfs_journal_writer::get_ith_buffer_block(size_t index)
{
    if (index >= journal_buffer.size())
        journal_buffer.resize(index + 1);
    return journal_buffer[index].get_ptr();
}

void hscfs_journal_writer::fill_buffer_with_nop(char *start, char *end)
{
    uint16_t length = end - start;
    if (length < sizeof(meta_journal_entry))
        throw std::invalid_argument("not enough memory to fill nop entry.");
    meta_journal_entry entry = {.len = length, .type = JOURNAL_TYPE_NOP};
    *reinterpret_cast<meta_journal_entry*>(start) = entry;
}

void hscfs_journal_writer::append_end_entry()
{
    char *p = get_ith_buffer_block(buffer_tail_idx);
    p += buffer_tail_off;
    meta_journal_entry entry = {.len = sizeof(meta_journal_entry), .type = JOURNAL_TYPE_END};
    *reinterpret_cast<meta_journal_entry*>(p) = entry;
}

void hscfs_journal_writer::async_write_callback(comm_cmd_result res, void *arg)
{
    async_vecio_synchronizer *syr = static_cast<async_vecio_synchronizer*>(arg);
    syr->cplt_once(res);
}

hscfs_journal_writer::hscfs_journal_writer(comm_dev *device, uint64_t journal_area_start_lpa, 
    uint64_t journal_area_end_lpa)
{
    start_lpa = journal_area_start_lpa;
    end_lpa = journal_area_end_lpa;
    cur_journal = nullptr;
    buffer_tail_idx = 0;
    buffer_tail_off = 0;
    dev = device;
}

uint64_t hscfs_journal_writer::collect_pending_journal_to_write_buffer()
{
    buffer_tail_idx = buffer_tail_off = 0;
    auto journal_vecs = journal_output_vec_generate();
    for (auto &entry: journal_vecs)
    {
        entry->generate_output_vector();
        entry->prepare_output();
        bool cplt_write_entry = false;
        while (!cplt_write_entry)
        {
            char *cur_buf_start_addr = get_ith_buffer_block(buffer_tail_idx);
            char *cur_buf_end_addr = cur_buf_start_addr + 4096;
            char *output_addr = cur_buf_start_addr + buffer_tail_off;

            journal_output_state state = entry->output_to_buffer(&output_addr, cur_buf_end_addr);
            switch (state)
            {
                /*
                 * 成功写入buffer，可能因缓存块空间不足只写了一部分，或已经全部写完
                 * 更新当前块内偏移到写入的尾后地址即可
                 */
            case journal_output_state::OK:
                buffer_tail_off = output_addr - cur_buf_start_addr;
                break;

                /*
                 * 当前buffer block空间不足，无法再存放日志条目
                 * 使用NOP进行填充
                 */
            case journal_output_state::NO_ENOUGH_BUFFER:
                fill_buffer_with_nop(output_addr, cur_buf_end_addr);
                buffer_tail_off = 4096;
                break;

                // 当前日志项已经写完
            case journal_output_state::REACH_END:
                cplt_write_entry = true;
                break;
            }

            // 如果当前缓存块已经写满，继续使用下一个缓存块
            if (buffer_tail_off == 4096)
            {
                buffer_tail_idx++;
                buffer_tail_off = 0;
            }
        }
    }
    append_end_entry();
    return buffer_tail_idx + 1;
}

void hscfs_journal_writer::write_to_SSD(uint64_t cur_tail)
{
    uint64_t io_num = buffer_tail_idx + 1;
    async_vecio_synchronizer syr(io_num);
    for (size_t i = 0; i <= buffer_tail_idx; ++i, ++cur_tail)
    {
        if (cur_tail == end_lpa)
            cur_tail = start_lpa;
        int ret = comm_submit_async_rw_request(dev, journal_buffer[i].get_ptr(), LPA_TO_LBA(cur_tail), 
            8, async_write_callback, &syr, COMM_IO_WRITE);
        if (ret != 0)
            throw hscfs_io_error("journal writer: submit async write failed.");
    }
    comm_cmd_result res = syr.wait_cplt();
    if (res != COMM_CMD_SUCCESS)
        throw hscfs_io_error("journal writer: error occurred in async write process.");
}
