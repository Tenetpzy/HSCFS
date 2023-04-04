#include "gtest/gtest.h"
#include "cache/SIT_NAT_cache.hh"
#include "fmt/ostream.h"

#include <vector>

TEST(sit_cache_test, 1)
{
    using namespace std;
    using namespace hscfs;
    vector<uint32_t> test_lpa_seq = {1, 2, 3, 4, 5, 6, 7, 8};
    SIT_NAT_cache cache(nullptr, 2);
    for (auto i: test_lpa_seq)
    {
        fmt::println(std::cout, "getting lpa {}", i);
        auto ret = cache.get_cache_entry(i);
        EXPECT_EQ(ret->get_lpa(), i);
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
        auto ret = cache.get_cache_entry(i);
        EXPECT_EQ(ret->get_lpa(), i);

        if (i & 1U)
        {
            fmt::println(std::cout, "pin lpa {}", i);
            ret->add_ref_count();
            cache.pin(ret->get_lpa());
        }
    }

    for (uint32_t i = 1; i < 8; i += 2)
    {
        auto ret = cache.get_cache_entry(i);
        EXPECT_EQ(ret->get_lpa(), i);
        EXPECT_EQ(ret->get_ref_count(), 1);
        ret->sub_ref_count();
        cache.unpin(ret->get_lpa());
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}