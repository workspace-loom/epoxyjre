/*
 * Copyright (c) 2026, the EpoxyJRE authors. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "gc/epoxy/epoxyPage.hpp"
#include "gc/epoxy/epoxyPageAllocator.hpp"
#include "gc/epoxy/epoxyRegion.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "memory/iterator.hpp"
#include "memory/memRegion.hpp"
#include "nmt/memTag.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "unittest.hpp"

static const size_t RegionPageSize = 64 * K;
static const size_t RegionPageWords = RegionPageSize / HeapWordSize;
static const uint   RegionNumPages = 64;

// Records the start and size of every object the region reports.
class RecordingClosure : public ObjectClosure {
public:
  GrowableArrayCHeap<HeapWord*, mtTest> _starts;
  GrowableArrayCHeap<size_t, mtTest>    _sizes;
  void do_object(oop obj) override {
    _starts.append(cast_from_oop<HeapWord*>(obj));
    _sizes.append(obj->size());
  }
};

class EpoxyRegionTest : public ::testing::Test {
protected:
  char* _base;
  EpoxyPageAllocator* _allocator;
  EpoxyRegion* _region;

  void SetUp() override {
    _base = os::reserve_memory_aligned(RegionPageSize * RegionNumPages, RegionPageSize, mtTest);
    ASSERT_NE(_base, (char*)nullptr);
    MemRegion reserved((HeapWord*)_base, (HeapWord*)(_base + RegionPageSize * RegionNumPages));
    _allocator = new EpoxyPageAllocator(reserved, RegionPageSize);
    _region = new EpoxyRegion(_allocator, "test");
  }

  void TearDown() override {
    delete _region;
    delete _allocator;
    os::release_memory(_base, RegionPageSize * RegionNumPages);
  }

  // Allocates and formats the memory as one object, so the region can be walked.
  HeapWord* allocate_object(size_t words) {
    HeapWord* p = _region->allocate(words);
    if (p != nullptr) {
      CollectedHeap::fill_with_object(p, words);
    }
    return p;
  }

  static size_t aligned_words(size_t words) {
    return align_up(MAX2(words, CollectedHeap::min_fill_size()), (size_t)MinObjAlignment);
  }
};

TEST_VM_F(EpoxyRegionTest, allocations_do_not_overlap) {
  const int Count = 10000;
  HeapWord** starts = NEW_C_HEAP_ARRAY(HeapWord*, Count, mtTest);
  size_t* sizes = NEW_C_HEAP_ARRAY(size_t, Count, mtTest);
  int allocated = 0;
  for (int i = 0; i < Count; i++) {
    size_t words = aligned_words(2 + (size_t)((i * 7919) % 199));
    HeapWord* p = allocate_object(words);
    if (p == nullptr) {
      break;  // region exhausted; everything allocated so far is still checked
    }
    starts[allocated] = p;
    sizes[allocated] = words;
    allocated++;
  }
  ASSERT_GT(allocated, 1000);

  for (int i = 0; i < allocated; i++) {
    EpoxyPage* page = _allocator->page_for(starts[i]);
    ASSERT_NE(page, (EpoxyPage*)nullptr);
    EXPECT_EQ(page->owner(), _region);
    EXPECT_GE(starts[i], page->bottom());
    EXPECT_LE(starts[i] + sizes[i], page->end());
  }

  // Allocation order within one region is address order per page, so sorting by
  // address and checking neighbours proves no two allocations overlap.
  for (int i = 1; i < allocated; i++) {
    for (int j = i; j > 0 && starts[j - 1] > starts[j]; j--) {
      swap(starts[j - 1], starts[j]);
      swap(sizes[j - 1], sizes[j]);
    }
  }
  for (int i = 1; i < allocated; i++) {
    ASSERT_LE(starts[i - 1] + sizes[i - 1], starts[i]) << "allocation " << i;
  }

  FREE_C_HEAP_ARRAY(starts);
  FREE_C_HEAP_ARRAY(sizes);
}

TEST_VM_F(EpoxyRegionTest, tail_is_left_unused_when_page_changes) {
  HeapWord* first = allocate_object(RegionPageWords - 10);
  HeapWord* second = allocate_object(20);
  ASSERT_NE(first, (HeapWord*)nullptr);
  ASSERT_NE(second, (HeapWord*)nullptr);
  EpoxyPage* first_page = _allocator->page_for(first);
  EXPECT_NE(first_page, _allocator->page_for(second));

  // The first page stops at the end of its only object; the walk sees exactly
  // the two objects and nothing in the 10 unused words.
  EXPECT_EQ(first_page->top(), first + (RegionPageWords - 10));
  RecordingClosure cl;
  _region->object_iterate(&cl);
  ASSERT_EQ(cl._starts.length(), 2);
  EXPECT_EQ(cl._starts.at(0), first);
  EXPECT_EQ(cl._sizes.at(0), RegionPageWords - 10);
  EXPECT_EQ(cl._starts.at(1), second);
  EXPECT_EQ(cl._sizes.at(1), (size_t)20);

  // A request that fits the abandoned tail does not go back to it.
  HeapWord* third = allocate_object(aligned_words(4));
  EXPECT_EQ(_allocator->page_for(third), _allocator->page_for(second));
}

TEST_VM_F(EpoxyRegionTest, large_request_gets_a_run) {
  size_t words = aligned_words(3 * RegionPageWords + 1);
  HeapWord* p = allocate_object(words);
  ASSERT_NE(p, (HeapWord*)nullptr);
  EpoxyPage* page = _allocator->page_for(p);
  ASSERT_NE(page, (EpoxyPage*)nullptr);
  EXPECT_EQ(page->run_length(), 4u);
  EXPECT_EQ(page->bottom(), p);
  EXPECT_EQ(page->top(), p + words);
  EXPECT_EQ(_allocator->page_for(p + words - 1), page);
  EXPECT_EQ(_region->used_bytes(), 4 * RegionPageSize);

  // The run holds exactly that one object.
  RecordingClosure cl;
  _region->object_iterate(&cl);
  ASSERT_EQ(cl._starts.length(), 1);
  EXPECT_EQ(cl._starts.at(0), p);
  EXPECT_EQ(cl._sizes.at(0), words);

  // Small allocation afterwards goes to an ordinary page, not into the run.
  HeapWord* small = allocate_object(aligned_words(4));
  ASSERT_NE(small, (HeapWord*)nullptr);
  EXPECT_NE(_allocator->page_for(small), page);
}

TEST_VM_F(EpoxyRegionTest, exhaustion_returns_null) {
  for (uint i = 0; i < RegionNumPages; i++) {
    ASSERT_NE(allocate_object(RegionPageWords), (HeapWord*)nullptr) << "page " << i;
  }
  EXPECT_EQ(_region->allocate(RegionPageWords), (HeapWord*)nullptr);
  EXPECT_EQ(_region->allocate(aligned_words(2)), (HeapWord*)nullptr);
  EXPECT_EQ(_region->allocate(2 * RegionPageWords), (HeapWord*)nullptr);
  EXPECT_EQ(_region->used_bytes(), RegionPageSize * RegionNumPages);
}

TEST_VM_F(EpoxyRegionTest, used_bytes_counts_whole_pages) {
  EXPECT_EQ(_region->used_bytes(), (size_t)0);
  ASSERT_NE(allocate_object(aligned_words(2)), (HeapWord*)nullptr);
  EXPECT_EQ(_region->used_bytes(), RegionPageSize);
  EXPECT_STREQ(_region->name(), "test");
}

// A request with a minimum and a desired size (a TLAB refill) takes what is
// left of the current page when that is at least the minimum.
TEST_VM_F(EpoxyRegionTest, ranged_request_takes_the_page_tail) {
  HeapWord* first = allocate_object(RegionPageWords - 100);
  ASSERT_NE(first, (HeapWord*)nullptr);

  size_t actual = 0;
  HeapWord* tail = _region->allocate(50, RegionPageWords, &actual);
  ASSERT_NE(tail, (HeapWord*)nullptr);
  EXPECT_EQ(actual, (size_t)100);
  EXPECT_EQ(tail, first + (RegionPageWords - 100));
  EXPECT_EQ(_region->used_bytes(), RegionPageSize);
}

TEST_VM_F(EpoxyRegionTest, ranged_request_moves_on_when_tail_is_below_minimum) {
  HeapWord* first = allocate_object(RegionPageWords - 100);
  ASSERT_NE(first, (HeapWord*)nullptr);

  size_t actual = 0;
  HeapWord* fresh = _region->allocate(200, RegionPageWords, &actual);
  ASSERT_NE(fresh, (HeapWord*)nullptr);
  EXPECT_EQ(actual, RegionPageWords);
  EXPECT_NE(_allocator->page_for(fresh), _allocator->page_for(first));
  EXPECT_EQ(_region->used_bytes(), 2 * RegionPageSize);
}

TEST_VM_F(EpoxyRegionTest, ranged_request_gets_desired_size_when_it_fits) {
  size_t actual = 0;
  HeapWord* p = _region->allocate(50, 400, &actual);
  ASSERT_NE(p, (HeapWord*)nullptr);
  EXPECT_EQ(actual, (size_t)400);
}

// A request too big for the current page's tail goes to a fresh page, but the
// region keeps bumping whichever of the two pages has more room left.
TEST_VM_F(EpoxyRegionTest, mid_size_request_does_not_abandon_a_roomier_page) {
  HeapWord* a = allocate_object(1000);
  HeapWord* b = allocate_object(RegionPageWords - 700);   // does not fit after a
  HeapWord* c = allocate_object(1000);                    // fits after a, not after b
  ASSERT_NE(a, (HeapWord*)nullptr);
  ASSERT_NE(b, (HeapWord*)nullptr);
  ASSERT_NE(c, (HeapWord*)nullptr);
  EXPECT_NE(_allocator->page_for(b), _allocator->page_for(a));
  EXPECT_EQ(_allocator->page_for(c), _allocator->page_for(a));
  EXPECT_EQ(c, a + 1000);
  EXPECT_EQ(_region->used_bytes(), 2 * RegionPageSize);

  // Both pages are still walked completely.
  RecordingClosure cl;
  _region->object_iterate(&cl);
  EXPECT_EQ(cl._starts.length(), 3);
}

// When the fresh page ends up with more room, the region moves to it.
TEST_VM_F(EpoxyRegionTest, mid_size_request_moves_to_the_fresh_page_when_it_is_roomier) {
  HeapWord* a = allocate_object(RegionPageWords - 500);
  HeapWord* b = allocate_object(1000);                    // does not fit after a
  HeapWord* c = allocate_object(1000);
  ASSERT_NE(b, (HeapWord*)nullptr);
  ASSERT_NE(c, (HeapWord*)nullptr);
  EXPECT_NE(_allocator->page_for(b), _allocator->page_for(a));
  EXPECT_EQ(c, b + 1000);
}
