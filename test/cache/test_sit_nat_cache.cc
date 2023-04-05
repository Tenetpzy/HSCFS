#include "gtest/gtest.h"
#include "fmt/ostream.h"
#include <vector>

#define private public
#define protected public
#include "cache/SIT_NAT_cache.hh"
#undef private
#undef protected

TEST(sit_cache_test, 1)
{
    using namespace std;
    using namespace hscfs;
    vector<uint32_t> test_lpa_seq = {1, 2, 3, 4, 5, 6, 7, 8};
    SIT_NAT_cache cache(nullptr, 2);
    for (auto i: test_lpa_seq)
    {
        fmt::println(std::cout, "getting lpa {}", i);
        auto handle = cache.get_cache_entry(i);
        EXPECT_EQ(handle.entry_->lpa_, i);
    }
}

TEST(sit_cache_test, 2)
{
    using namespace std;
    using namespace hscfs;
    vector<uint32_t> test_lpa_seq = {1, 2, 3, 4, 5, 6, 7, 8};
    SIT_NAT_cache cache(nullptr, 2);

    for (auto i: test_lpa_seq)
    {
        fmt::println(std::cout, "getting lpa {}", i);
        auto handle = cache.get_cache_entry(i);
        EXPECT_EQ(handle.entry_->lpa_, i);

        if (i & 1U)
        {
            fmt::println(std::cout, "pin lpa {}", i);
            handle.add_host_version();
        }
    }

    for (uint32_t i = 1; i < 8; i += 2)
    {
        auto handle = cache.get_cache_entry(i);
        EXPECT_EQ(handle.entry_->lpa_, i);
        EXPECT_EQ(handle.entry_->ref_count, 2);
        handle.add_SSD_version();
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}