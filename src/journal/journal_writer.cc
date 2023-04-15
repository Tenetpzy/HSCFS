#include <unordered_map>
#include <map>

#include "journal/journal_writer.hh"
#include "journal/journal_container.hh"
#include "journal/journal_type.h"
#include "communication/memory.h"
#include "communication/comm_api.h"
#include "utils/hscfs_exceptions.hh"
#include "utils/io_utils.hh"
#include "utils/hscfs_log.h"

namespace hscfs {

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
     * 留下NOP空间指至少留下一个日志项首部长度。
     */
    static uint64_t generic_calculate_writable_entry_num(uint64_t buffer_size, uint64_t entry_size, 
        uint64_t expected_write_num)
    {
        uint64_t ret;
        const uint64_t entry_header_len = sizeof(meta_journal_entry);
        uint64_t expected_write_len = entry_header_len + expected_write_num * entry_size;
        
        // 若期待写的长度小于buffer长度
        if (expected_write_len < buffer_size)
        {
            // 如果全部写完，还能至少剩下header的空间（即NOP的空间），则可以全部写完
            if (buffer_size - expected_write_len >= entry_header_len)
                ret = expected_write_num;
            
            // 如果全部写完无法留出NOP了，就计算最多能写多少个
            // buffer长度减去NOP和该日志项首部(2 * entry_header_len)，就是能够存放日志条目的最大空间
            else if (buffer_size > 2 * entry_header_len)
                ret = (buffer_size - 2 * entry_header_len) / entry_size;
            else
                ret = 0;
        }

        // 若buffer长度正好等于期待写的长度，则正好全部写完
        else if (expected_write_len == buffer_size)
            ret = expected_write_num;
        
        // buffer长度小于期待写长度
        else
        {
            // buffer已经不够放一个首部了，不能再写
            // 这种情况理论上不应该出现，因为需始终遵守留出NOP空间（最小即是首部）的协议
            if (buffer_size < entry_header_len)
                ret = 0;
            
            // 计算最大能写的日志条目个数(buffer_size - entry_header_len) / entry_size
            // 递归计算，以考虑NOP留空处理。递归时只可能进入前两个分支
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
        size_t journal_entry_size = sizeof(journalT);
        if (rest_output_num == 0)
            return journal_output_state::REACH_END;
        
        size_t output_num = generic_calculate_writable_entry_num(end_addr - *p_start_addr, 
            journal_entry_size, rest_output_num);
        if (output_num == 0)
            return journal_output_state::NO_ENOUGH_BUFFER;
        
        char *p = *p_start_addr;
        meta_journal_entry header;
        header.len = sizeof(meta_journal_entry) + output_num * journal_entry_size;
        header.type = _journal_type;
        *reinterpret_cast<meta_journal_entry*>(p) = header;
        journalT *entry = reinterpret_cast<journalT*>(p + sizeof(meta_journal_entry));

        for (size_t i = 0; i < output_num; ++i, ++output_it, ++entry)
            *entry = journal[output_it->second];
        
        rest_output_num -= output_num;
        *p_start_addr = reinterpret_cast<char*>(entry);
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

std::vector<std::unique_ptr<journal_output_vector>> journal_writer::journal_output_vec_generate() const
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

char *journal_writer::get_ith_buffer_block(size_t index)
{
    if (index >= journal_buffer.size())
        journal_buffer.resize(index + 1);
    return journal_buffer[index].get_ptr();
}

void journal_writer::fill_buffer_with_nop(char *start, char *end)
{
    uint16_t length = end - start;
    if (length < sizeof(meta_journal_entry))
        throw std::invalid_argument("not enough memory to fill nop entry.");
    meta_journal_entry entry = {.len = length, .type = JOURNAL_TYPE_NOP};
    *reinterpret_cast<meta_journal_entry*>(start) = entry;
}

void journal_writer::append_end_entry()
{
    char *p = get_ith_buffer_block(buffer_tail_idx);
    p += buffer_tail_off;
    meta_journal_entry entry = {.len = sizeof(meta_journal_entry), .type = JOURNAL_TYPE_END};
    *reinterpret_cast<meta_journal_entry*>(p) = entry;
}

void journal_writer::async_write_callback(comm_cmd_result res, void *arg)
{
    async_vecio_synchronizer *syr = static_cast<async_vecio_synchronizer*>(arg);
    syr->cplt_once(res);
}

journal_writer::journal_writer(comm_dev *device, uint64_t journal_area_start_lpa, 
    uint64_t journal_area_end_lpa)
{
    start_lpa = journal_area_start_lpa;
    end_lpa = journal_area_end_lpa;
    cur_journal = nullptr;
    buffer_tail_idx = 0;
    buffer_tail_off = 0;
    dev = device;
}

#ifdef CONFIG_PRINT_DEBUG_INFO
void print_journal_block(const char *start);
#endif

uint64_t journal_writer::collect_pending_journal_to_write_buffer()
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

void journal_writer::write_to_SSD(uint64_t cur_tail)
{
    uint64_t io_num = buffer_tail_idx + 1;
    async_vecio_synchronizer syr(io_num);
    for (size_t i = 0; i <= buffer_tail_idx; ++i, ++cur_tail)
    {
        if (cur_tail == end_lpa)
            cur_tail = start_lpa;

        #ifdef CONFIG_PRINT_DEBUG_INFO
        HSCFS_LOG(HSCFS_LOG_INFO, "journal writer: journal in %lu th buffer block which will be written to SSD:\n", i);
        print_journal_block(get_ith_buffer_block(i));
        #endif

        int ret = comm_submit_async_rw_request(dev, journal_buffer[i].get_ptr(), LPA_TO_LBA(cur_tail), 
            LBA_PER_LPA, async_write_callback, &syr, COMM_IO_WRITE);
        if (ret != 0)
            throw io_error("journal writer: submit async write failed.");
    }
    comm_cmd_result res = syr.wait_cplt();
    if (res != COMM_CMD_SUCCESS)
        throw io_error("journal writer: error occurred in async write process.");
}

}  // namespace hscfs