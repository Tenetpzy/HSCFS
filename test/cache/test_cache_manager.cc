#include "cache/cache_manager.hh"
#include "gtest/gtest.h"

#include <vector>

using namespace hscfs;
using std::unique_ptr;
using std::vector;

struct cache_obj
{
    int k, v;
    cache_obj(int k_ = 0, int v_ = 0) {
        k = k_;
        v = v_;
    }
};

class test_cache_manager: public ::testing::Test
{
protected:
    using cache_manager_t = generic_cache_manager<int, cache_obj, cache_hash_index, lru_replacer>;

    void SetUp() override
    {
        cache_manager.reset(new cache_manager_t);
    }

protected:
    unique_ptr<cache_manager_t> cache_manager;
};

TEST_F(test_cache_manager, basic_function)
{
    unique_ptr<cache_obj> p1(new cache_obj);
    int test_key = 1, test_value = 10;
    p1->k = test_key;
    p1->v = test_value;
    cache_manager->add(p1->k, p1);
    auto p2 = cache_manager->get(test_key);
    EXPECT_EQ(p2->v, test_value);
    auto p3 = cache_manager->get(2);
    EXPECT_EQ(p3, nullptr);
    cache_manager->pin(test_key);
    auto p4 = cache_manager->replace_one();
    EXPECT_EQ(p4, nullptr);
    cache_manager->unpin(test_key);
    auto p5 = cache_manager->replace_one();
    EXPECT_EQ(p5->k, test_key);
    EXPECT_EQ(p5->v, test_value);
}

TEST_F(test_cache_manager, lru)
{
    vector<unique_ptr<cache_obj>> v;
    for (int i = 0; i < 10; ++i)
        v.emplace_back(std::make_unique<cache_obj>(i, i));
    for (int i = 0; i < 10; ++i)
        cache_manager->add(i, v[i]);
    for (int i = 9; i >= 0; --i)
        cache_manager->get(i);
    for (int i = 0; i < 10; ++i)
    {
        auto p = cache_manager->replace_one();
        EXPECT_EQ(p->k, 9 - i);
    }
}

TEST_F(test_cache_manager, lru_with_pin)
{
    vector<unique_ptr<cache_obj>> v;
    for (int i = 0; i < 10; ++i)
        v.emplace_back(std::make_unique<cache_obj>(i, i));
    for (int i = 0; i < 10; ++i)
        cache_manager->add(i, v[i]);
    for (int i = 0; i < 10; ++i)
    {
        cache_manager->get(i);
        if (i & 1)
            cache_manager->pin(i);
    }
    for (int i = 0; ; i += 2)
    {
        unique_ptr<cache_obj> p = cache_manager->replace_one();
        if (p == nullptr)
        {
            EXPECT_EQ(i, 10);
            break;
        }
        EXPECT_EQ(p->k, i);
    }
    for (int i = 1; i < 10; i += 2)
        cache_manager->unpin(i);
    for (int i = 1; i < 10; i += 2)
    {
        unique_ptr<cache_obj> p = cache_manager->replace_one();
        EXPECT_EQ(p->k, i);
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}