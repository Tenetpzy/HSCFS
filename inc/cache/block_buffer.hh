#pragma once

#include "utils/declare_utils.hh"

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

    /*
     * 将buf指向的4KB块拷贝到block_buffer中
     * file mapping查询时使用连续缓存区保存多个块，
     * 要将它们插入node block缓存，目前的设计只能进行拷贝
     */
    void copy_content_from_buf(char *buf);
    ~block_buffer();

    char *get_ptr() noexcept
    {
        return buffer;
    }

private:
    char *buffer;
};

}  // namespace hscfs