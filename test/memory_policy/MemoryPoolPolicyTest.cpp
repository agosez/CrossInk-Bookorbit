#include <Memory.h>
#include <PoolBudget.h>
#include <gtest/gtest.h>

struct MemoryPoolPolicyTest : testing::Test {
  void SetUp() override { fakeheap::reset(); }
  void TearDown() override { EXPECT_TRUE(fakeheap::live.empty()); }
};
TEST_F(MemoryPoolPolicyTest, OverflowAndFragmentationCannotAdmitOperation) {
  using namespace MemoryBudget;
  EXPECT_FALSE(admits({SIZE_MAX, SIZE_MAX, SIZE_MAX}, {SIZE_MAX, 1, 1}));
  EXPECT_TRUE(admits({SIZE_MAX, SIZE_MAX, SIZE_MAX}, {SIZE_MAX, SIZE_MAX, 0}));
  EXPECT_FALSE(admits({1000, 1000, 10}, {100, 100, 0}));
  EXPECT_FALSE(admitsOperation({20, 20, 20}, {1000, 1000, 1000}, {21, 1, 0}, {0, 0, 0}));
  EXPECT_FALSE(admitsOperation({1000, 1000, 1000}, {20, 20, 20}, {0, 0, 0}, {21, 1, 0}));
}
TEST_F(MemoryPoolPolicyTest, ExplicitPoolsAndCorrectDeallocation) {
  fakeheap::defaultExternal = true;
  auto internal = makeInternalByteBufferNoThrow(100);
  auto external = makePsramByteBufferNoThrow(100);
  auto defaults = makeHeapByteBufferNoThrow(100);
  EXPECT_EQ(byteBufferPool(internal.get()), MemoryPool::Internal);
  EXPECT_EQ(byteBufferPool(external.get()), MemoryPool::Psram);
  EXPECT_EQ(byteBufferPool(defaults.get()), MemoryPool::Psram);
  EXPECT_EQ(fakeheap::live.size(), 3u);
}
TEST_F(MemoryPoolPolicyTest, AdmissionDoesNotGuaranteeAllocation) {
  ASSERT_TRUE(MemoryBudget::canAllocatePsram(100));
  fakeheap::external.fail = 1;
  EXPECT_FALSE(makePsramByteBufferNoThrow(100));
  fakeheap::reset(false);
  EXPECT_FALSE(psramHeapAvailable());
  EXPECT_FALSE(MemoryBudget::canAllocatePsram(1));
  EXPECT_FALSE(makePsramByteBufferNoThrow(1));
  EXPECT_FALSE(makeInternalByteBufferNoThrow(0));
}

#include <Arena.h>
#include <ArenaVector.h>
TEST_F(MemoryPoolPolicyTest, ArenaAlignmentGrowthCheckpointAndReuse) {
  Arena a(ArenaBacking::PsramOnly);
  ASSERT_TRUE(a.init(64));
  ASSERT_NE(a.alloc(1, 1), nullptr);
  auto* aligned = a.alloc(sizeof(std::max_align_t));
  ASSERT_NE(aligned, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(aligned) % alignof(std::max_align_t), 0u);
  auto cp = a.save();
  ASSERT_NE(a.alloc(100), nullptr);
  ASSERT_NE(a.alloc(200), nullptr);
  EXPECT_EQ(fakeheap::live.size(), 3u);
  a.restore(cp);
  EXPECT_EQ(fakeheap::live.size(), 1u);
  EXPECT_EQ(a.current->offset, cp.offset);
  a.clear();
  EXPECT_EQ(a.used(), 0u);
  ASSERT_NE(a.alloc(64), nullptr);
  EXPECT_EQ(a.capacityInPool(MemoryPool::Internal), 0u);
  EXPECT_GT(a.capacityInPool(MemoryPool::Psram), 64u);
}
TEST_F(MemoryPoolPolicyTest, ArenaFailuresAndMixedFallbackDoNotLeak) {
  Arena a(ArenaBacking::PsramPreferred);
  fakeheap::external.fail = 1;
  ASSERT_TRUE(a.init(64));
  EXPECT_EQ(a.head->pool, MemoryPool::Internal);
  ASSERT_NE(a.alloc(128), nullptr);
  EXPECT_EQ(a.current->pool, MemoryPool::Psram);
  auto cp = a.save();
  fakeheap::external.fail = 1;
  fakeheap::internal.largest = 1;
  EXPECT_EQ(a.alloc(1000), nullptr);
  EXPECT_EQ(a.current, cp.slab);
  EXPECT_EQ(fakeheap::live.size(), 2u);
  a.release();
  Arena optional(ArenaBacking::PsramOnly);
  fakeheap::external.fail = 1;
  EXPECT_FALSE(optional.init(100));
  EXPECT_EQ(optional.alloc(10), nullptr);
  EXPECT_TRUE(fakeheap::live.empty());
}
TEST_F(MemoryPoolPolicyTest, ArenaRejectsOverflowAndUnsupportedAlignment) {
  Arena a;
  EXPECT_FALSE(a.init(SIZE_MAX));
  ASSERT_TRUE(a.init(64));
  EXPECT_EQ(a.alloc(SIZE_MAX), nullptr);
  EXPECT_EQ(a.alloc(10, 0), nullptr);
  EXPECT_EQ(a.alloc(10, 3), nullptr);
  EXPECT_EQ(a.alloc(10, alignof(std::max_align_t) * 2), nullptr);
  EXPECT_EQ(arenaNewArray<uint64_t>(a, SIZE_MAX), nullptr);
  ArenaVector<uint64_t> v(a);
  EXPECT_FALSE(v.reserve(SIZE_MAX));
  EXPECT_EQ(fakeheap::live.size(), 1u);
  ASSERT_TRUE(a.init(32));  // reinit releases the old slab
  EXPECT_EQ(fakeheap::live.size(), 1u);
}
