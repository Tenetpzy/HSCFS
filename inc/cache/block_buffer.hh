#pragma once

#include "utils/declare_utils.hh"
#include "communication/comm_api.h"
#include <cstdint>

struct comm_dev;

namespace hscfs {

/* 块缓存区的容器，构造时分配4KB块缓存区，析构时释放 */
class block_buffer
{
public:
    block_buffer();
    block_buffer(const block_buffer&);
    block_buffer(block_buffer&&) noexcept;
    block_buffer &operator=(const block_buffer&);
    block_buffer &operator=(block_buffer&&) noexcept;
    ~block_buffer();

    /*
     * 将buf指向的4KB块拷贝到block_buffer中
     * file mapping查询时使用连续缓存区保存多个块，
     * 要将它们插入node block缓存，目前的设计只能进行拷贝
     */
    void copy_content_from_buf(char *buf);

    /* 从SSD读lpa到buffer中，同步，完成读操作后返回 */
    void read_from_lpa(comm_dev *dev, uint32_t lpa);

    /* 把buffer中的内容写入lpa，同步，完成写操作后返回 */
    void write_to_lpa_sync(comm_dev *dev, uint32_t lpa);

    /* 把buffer中的内容写入lpa，异步，立刻返回 */
    void write_to_lpa_async(comm_dev *dev, uint32_t lpa, comm_async_cb_func cb_func, void *cb_arg);

    char *get_ptr() noexcept
    {
        return buffer;
    }

private:
    char *buffer;
};

}  // namespace hscfs