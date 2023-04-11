#include "gtest/gtest.h"

#define private public
#define protected public
#include "cache/node_block_cache.hh"
#undef private
#undef protected

TEST(node_cache, 1)
{
    using namespace hscfs;
    node_block_cache cache(1);  // 缓存项数量阈值为1个

    {
        block_buffer buf1;
        auto handle1 = cache.add(std::move(buf1), 1, INVALID_NID, 1);
        EXPECT_EQ(handle1.entry->ref_count, 1);
        EXPECT_EQ(handle1.entry->state, node_block_cache_entry_state::uptodate);
        EXPECT_EQ(handle1.entry->old_lpa, 1);

        {
            /*
            * 索引树结构，数字为nid，箭头指向parent
            * 1 <- 2
            */
            block_buffer buf2;
            auto handle2 = cache.add(std::move(buf2), 2, 1, 2);
            EXPECT_EQ(handle1.entry->ref_count, 2);

            /*
            * 1 <- 2
            * 1 <- 3
            */
            block_buffer buf3;
            auto handle3 = cache.add(std::move(buf3), 3, 1, 3);
            EXPECT_EQ(handle1.entry->ref_count, 3);

            {
                /*
                * 1 <- 2
                * 1 <- 3
                * 2 <- 4
                */
                block_buffer buf4;
                auto handle4 = cache.add(std::move(buf4), 4, 2, 4);
                EXPECT_EQ(handle2.entry->ref_count, 2);
            }
            
            /*
            * 1 <- 2
            * 1 <- 3
            * 4被换出
            */
            cache.force_replace();
            EXPECT_EQ(handle2.entry->ref_count, 1);

            // 让3不被换出
            handle3.add_host_version();
            handle3.mark_dirty();
            EXPECT_EQ(handle3.entry->ref_count, 3);
        }
        
        {
            /*
            * 1 <- 3
            * 2被换出
            */
            cache.force_replace();
            EXPECT_EQ(handle1.entry->ref_count, 2);
            auto handle3 = cache.get(3);
            EXPECT_EQ(handle3.entry->ref_count, 3);
            EXPECT_EQ(handle3.entry->state, node_block_cache_entry_state::dirty);
            
            // 让3可以被换出
            {
                handle3.add_SSD_version();
                auto dirty_list = cache.get_dirty_list();
                EXPECT_EQ(dirty_list.size(), 1);
                EXPECT_EQ(dirty_list[0].entry, handle3.entry);
                cache.clear_dirty_list();
            }

            EXPECT_EQ(handle3.entry->ref_count, 1);
            EXPECT_EQ(handle3.entry->state, node_block_cache_entry_state::uptodate);
        }

        // 3被换出，缓存中只剩1
        cache.force_replace();
        EXPECT_EQ(handle1.entry->ref_count, 1);
    }

    node_block_cache_entry_handle tmp_handle;
    {
        {
            auto handle1 = cache.get(1);
            EXPECT_EQ(handle1.entry->ref_count, 1);
        }
        
        // 加入5，让1自动被换出
        block_buffer buf5;
        auto handle5 = cache.add(std::move(buf5), 5, INVALID_NID, 5);
        tmp_handle = handle5;
        EXPECT_EQ(handle5.entry->ref_count, 2);
    }

    EXPECT_EQ(cache.get(1).is_empty(), true);
    EXPECT_EQ(tmp_handle.entry->ref_count, 1);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}