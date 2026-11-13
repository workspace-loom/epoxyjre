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
#include "memory/memRegion.hpp"
#include "nmt/memTag.hpp"
#include "runtime/os.hpp"
#include "threadHelper.inline.hpp"
#include "utilities/globalDefinitions.hpp"
#include "unittest.hpp"

// 64 pages of 64 KB, reserved (not committed) for the duration of one test.
static const size_t PageSize = 64 * K;
static const uint   NumPages = 64;

class EpoxyPageAllocatorTest : public ::testing::Test {
protected:
  char* _base;
  EpoxyPageAllocator* _allocator;

  void SetUp() override {
    _base = os::reserve_memory_aligned(PageSize * NumPages, PageSize, mtTest);
    ASSERT_NE(_base, (char*)nullptr);
    MemRegion reserved((HeapWord*)_base, (HeapWord*)(_base + PageSize * NumPages));
    _allocator = new EpoxyPageAllocator(reserved, PageSize);
  }

  void TearDown() override {
    delete _allocator;
    os::release_memory(_base, PageSize * NumPages);
  }

  static size_t byte_size(EpoxyPage* page) {
    return pointer_delta(page->end(), page->bottom(), 1);
  }
};

TEST_VM_F(EpoxyPageAllocatorTest, single_pages_are_distinct_and_aligned) {
  EpoxyPage* pages[NumPages];
  for (uint i = 0; i < NumPages; i++) {
    pages[i] = _allocator->allocate(1);
    ASSERT_NE(pages[i], (EpoxyPage*)nullptr) << "page " << i;
    EXPECT_EQ((uintptr_t)pages[i]->bottom() % PageSize, (uintptr_t)0);
    EXPECT_EQ(pages[i]->run_length(), 1u);
    EXPECT_EQ(pages[i]->top(), pages[i]->bottom());
    EXPECT_EQ(byte_size(pages[i]), PageSize);
    for (uint j = 0; j < i; j++) {
      EXPECT_NE(pages[i]->bottom(), pages[j]->bottom());
    }
  }
  EXPECT_EQ(_allocator->allocate(1), (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->used_pages(), (size_t)NumPages);
  EXPECT_EQ(_allocator->max_pages(), (size_t)NumPages);
  EXPECT_EQ(_allocator->page_size_bytes(), PageSize);
}

TEST_VM_F(EpoxyPageAllocatorTest, page_memory_is_writable) {
  EpoxyPage* page = _allocator->allocate(1);
  ASSERT_NE(page, (EpoxyPage*)nullptr);
  char* p = (char*)page->bottom();
  memset(p, 0x5A, PageSize);
  for (size_t i = 0; i < PageSize; i++) {
    ASSERT_EQ(p[i], (char)0x5A) << "byte " << i;
  }
  EXPECT_GE(_allocator->committed_bytes(), PageSize);
}

TEST_VM_F(EpoxyPageAllocatorTest, run_is_contiguous) {
  EpoxyPage* run = _allocator->allocate(5);
  ASSERT_NE(run, (EpoxyPage*)nullptr);
  EXPECT_EQ(byte_size(run), 5 * PageSize);
  EXPECT_EQ(run->run_length(), 5u);
  EXPECT_EQ(_allocator->used_pages(), (size_t)5);
  memset((char*)run->bottom(), 1, 5 * PageSize);
}

TEST_VM_F(EpoxyPageAllocatorTest, free_makes_pages_reusable) {
  EpoxyPage* pages[NumPages];
  for (uint i = 0; i < NumPages; i++) {
    pages[i] = _allocator->allocate(1);
    ASSERT_NE(pages[i], (EpoxyPage*)nullptr);
  }
  EXPECT_EQ(_allocator->used_pages(), (size_t)NumPages);
  for (uint i = 0; i < NumPages; i++) {
    _allocator->free(pages[i]);
  }
  EXPECT_EQ(_allocator->used_pages(), (size_t)0);
  EpoxyPage* all = _allocator->allocate(NumPages);
  ASSERT_NE(all, (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->used_pages(), (size_t)NumPages);
}

TEST_VM_F(EpoxyPageAllocatorTest, freed_singles_coalesce_into_a_run) {
  EpoxyPage* pages[NumPages];
  for (uint i = 0; i < NumPages; i++) {
    pages[i] = _allocator->allocate(1);
    ASSERT_NE(pages[i], (EpoxyPage*)nullptr);
  }
  // Free in a fixed shuffled order: 37 is coprime with 64, so this visits every index once.
  for (uint i = 0; i < NumPages; i++) {
    _allocator->free(pages[(i * 37 + 11) % NumPages]);
  }
  EpoxyPage* all = _allocator->allocate(NumPages);
  ASSERT_NE(all, (EpoxyPage*)nullptr);
  EXPECT_EQ(all->run_length(), NumPages);
}

TEST_VM_F(EpoxyPageAllocatorTest, page_for_maps_interior_addresses) {
  EpoxyPage* single = _allocator->allocate(1);
  EpoxyPage* run = _allocator->allocate(3);
  ASSERT_NE(single, (EpoxyPage*)nullptr);
  ASSERT_NE(run, (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->page_for((char*)single->bottom() + 1), single);
  EXPECT_EQ(_allocator->page_for(run->bottom()), run);
  EXPECT_EQ(_allocator->page_for((char*)run->end() - 1), run);
  EXPECT_EQ(_allocator->page_for(_base - 1), (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->page_for(_base + PageSize * NumPages), (EpoxyPage*)nullptr);
}

TEST_VM_F(EpoxyPageAllocatorTest, run_too_large_returns_null) {
  EpoxyPage* one = _allocator->allocate(1);
  ASSERT_NE(one, (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->allocate(NumPages + 1), (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->allocate(NumPages), (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->used_pages(), (size_t)1);
}

TEST_VM_F(EpoxyPageAllocatorTest, concurrent_allocate_and_free) {
  const int Threads = 8;
  const int Iterations = 10000;
  EpoxyPageAllocator* allocator = _allocator;
  volatile int failures = 0;
  volatile int* failures_addr = &failures;

  auto worker = [allocator, failures_addr](Thread* thread, int id) {
    for (int i = 0; i < Iterations; i++) {
      EpoxyPage* page = allocator->allocate(1);
      if (page == nullptr) {
        // 8 threads over 64 pages: a page must always be available.
        AtomicAccess::inc(failures_addr);
        continue;
      }
      int* words = (int*)page->bottom();
      words[0] = id;
      words[1] = i;
      os::naked_yield();
      if (words[0] != id || words[1] != i) {
        AtomicAccess::inc(failures_addr);
      }
      allocator->free(page);
    }
  };

  TestThreadGroup<decltype(worker)> group(worker, Threads);
  group.doit();
  group.join();

  EXPECT_EQ(failures, 0);
  EXPECT_EQ(_allocator->used_pages(), (size_t)0);
}

// ---- line table ----

static const size_t TestLinesPerPage = PageSize / EpoxyLineSize;
static const size_t TestLineWords = EpoxyLineSize / HeapWordSize;

TEST_VM_F(EpoxyPageAllocatorTest, lines_of_a_fresh_page_are_kept) {
  EpoxyPage* page = _allocator->allocate(2);
  ASSERT_NE(page, (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->lines_per_page(), TestLinesPerPage);
  for (size_t l = 0; l < 2 * TestLinesPerPage; l++) {
    EXPECT_EQ((uint)_allocator->line_state(page->bottom() + l * TestLineWords), (uint)EpoxyLineKept) << "line " << l;
  }
  EXPECT_FALSE(_allocator->is_scratch(page->bottom()));
  EXPECT_FALSE(_allocator->is_scratch(_base - 1));
}

TEST_VM_F(EpoxyPageAllocatorTest, scratch_is_bit_zero_of_the_line_state) {
  EpoxyPage* run = _allocator->allocate(3);
  ASSERT_NE(run, (EpoxyPage*)nullptr);
  _allocator->set_scratch(run, true);
  // Every line of every page of the run.
  EXPECT_TRUE(_allocator->is_scratch(run->bottom()));
  EXPECT_TRUE(_allocator->is_scratch((char*)run->end() - 1));
  EXPECT_EQ((uint)_allocator->line_state(run->bottom()), (uint)EpoxyLineScratch);

  // A range that starts and ends inside lines marks every line it touches.
  char* start = (char*)run->bottom() + 5 * EpoxyLineSize + EpoxyLineSize / 2;
  char* end = (char*)run->bottom() + 7 * EpoxyLineSize + 1;
  _allocator->mark_lines_promoted(start, end);
  EXPECT_EQ((uint)_allocator->line_state(run->bottom() + 4 * TestLineWords), (uint)EpoxyLineScratch);
  for (size_t l = 5; l <= 7; l++) {
    EXPECT_EQ((uint)_allocator->line_state(run->bottom() + l * TestLineWords), (uint)EpoxyLineScratchPromoted);
    EXPECT_TRUE(_allocator->is_scratch(run->bottom() + l * TestLineWords));
  }
  EXPECT_EQ((uint)_allocator->line_state(run->bottom() + 8 * TestLineWords), (uint)EpoxyLineScratch);
  // A range that ends exactly on a line boundary does not touch the next line.
  _allocator->mark_lines_promoted((char*)run->bottom() + 20 * EpoxyLineSize, (char*)run->bottom() + 21 * EpoxyLineSize);
  EXPECT_EQ((uint)_allocator->line_state(run->bottom() + 20 * TestLineWords), (uint)EpoxyLineScratchPromoted);
  EXPECT_EQ((uint)_allocator->line_state(run->bottom() + 21 * TestLineWords), (uint)EpoxyLineScratch);

  _allocator->set_scratch(run, false);
  EXPECT_FALSE(_allocator->is_scratch(run->bottom() + 6 * TestLineWords));
  _allocator->set_scratch(run, true);
  _allocator->free(run);
  // Freed memory is no longer scratch, whatever state its lines had.
  EXPECT_FALSE(_allocator->is_scratch(run->bottom() + 6 * TestLineWords));
  EXPECT_FALSE(_allocator->is_scratch(run->bottom() + 2 * TestLinesPerPage * TestLineWords));
}

TEST_VM_F(EpoxyPageAllocatorTest, release_page_frees_a_page_with_nothing_kept) {
  EpoxyPage* page = _allocator->allocate(1);
  ASSERT_NE(page, (EpoxyPage*)nullptr);
  _allocator->set_scratch(page, true);
  EpoxyPageRelease r;
  _allocator->release_page(page, &r);
  EXPECT_TRUE(r.freed);
  EXPECT_EQ(r.lines_freed, TestLinesPerPage);
  EXPECT_EQ(r.lines_kept, (size_t)0);
  EXPECT_EQ(_allocator->used_pages(), (size_t)0);
  EXPECT_FALSE(_allocator->is_scratch(page->bottom()));
}

TEST_VM_F(EpoxyPageAllocatorTest, release_page_converts_lines_and_reports_the_largest_hole) {
  EpoxyPage* page = _allocator->allocate(1);
  ASSERT_NE(page, (EpoxyPage*)nullptr);
  _allocator->set_scratch(page, true);
  // Promoted lines 3 and 4, and 100.
  _allocator->mark_lines_promoted(page->bottom() + 3 * TestLineWords, page->bottom() + 5 * TestLineWords);
  _allocator->mark_lines_promoted(page->bottom() + 100 * TestLineWords, page->bottom() + 101 * TestLineWords);

  EpoxyPageRelease r;
  _allocator->release_page(page, &r);
  EXPECT_FALSE(r.freed);
  EXPECT_EQ(r.lines_kept, (size_t)3);
  EXPECT_EQ(r.lines_freed, TestLinesPerPage - 3);
  EXPECT_EQ(r.max_hole_lines, TestLinesPerPage - 101);   // after line 100
  EXPECT_EQ(_allocator->used_pages(), (size_t)1);
  for (size_t l = 0; l < TestLinesPerPage; l++) {
    bool kept = (l == 3 || l == 4 || l == 100);
    EXPECT_EQ((uint)_allocator->line_state(page->bottom() + l * TestLineWords),
              kept ? (uint)EpoxyLineKept : (uint)EpoxyLineFree) << "line " << l;
  }
  EXPECT_FALSE(_allocator->is_scratch(page->bottom() + 3 * TestLineWords));
  EXPECT_FALSE(_allocator->is_scratch(page->bottom() + 10 * TestLineWords));   // free is not scratch
}

TEST_VM_F(EpoxyPageAllocatorTest, find_and_take_hole) {
  EpoxyPage* page = _allocator->allocate(1);
  ASSERT_NE(page, (EpoxyPage*)nullptr);
  _allocator->set_scratch(page, true);
  // Kept: 2, 6, 7; the holes are lines 0-1 (2), 3-5 (3) and 8 to the end.
  size_t promoted[] = {2, 6, 7};
  for (size_t l : promoted) {
    _allocator->mark_lines_promoted(page->bottom() + l * TestLineWords, page->bottom() + (l + 1) * TestLineWords);
  }
  EpoxyPageRelease r;
  _allocator->release_page(page, &r);
  ASSERT_FALSE(r.freed);

  HeapWord* start = nullptr;
  HeapWord* end = nullptr;
  ASSERT_TRUE(_allocator->find_hole(page->bottom(), page->end(), 1, &start, &end));
  EXPECT_EQ(start, page->bottom());
  EXPECT_EQ(end, page->bottom() + 2 * TestLineWords);
  ASSERT_TRUE(_allocator->find_hole(page->bottom(), page->end(), 3, &start, &end));   // skips the 2-line hole
  EXPECT_EQ(start, page->bottom() + 3 * TestLineWords);
  EXPECT_EQ(end, page->bottom() + 6 * TestLineWords);
  ASSERT_TRUE(_allocator->find_hole(page->bottom() + 6 * TestLineWords, page->end(), 4, &start, &end));
  EXPECT_EQ(start, page->bottom() + 8 * TestLineWords);
  EXPECT_EQ(end, page->end());
  EXPECT_FALSE(_allocator->find_hole(page->bottom(), page->end(), TestLinesPerPage, &start, &end));

  // Taking a hole turns its lines to scratch; it is no longer found.
  _allocator->take_hole(page->bottom() + 3 * TestLineWords, page->bottom() + 6 * TestLineWords);
  EXPECT_TRUE(_allocator->is_scratch(page->bottom() + 3 * TestLineWords));
  EXPECT_TRUE(_allocator->is_scratch(page->bottom() + 5 * TestLineWords));
  EXPECT_FALSE(_allocator->is_scratch(page->bottom() + 6 * TestLineWords));
  ASSERT_TRUE(_allocator->find_hole(page->bottom(), page->end(), 3, &start, &end));
  EXPECT_EQ(start, page->bottom() + 8 * TestLineWords);
}

TEST_VM_F(EpoxyPageAllocatorTest, recyclable_list_hands_each_page_out_once) {
  EpoxyPage* a = _allocator->allocate(1);
  EpoxyPage* b = _allocator->allocate(1);
  ASSERT_NE(a, (EpoxyPage*)nullptr);
  ASSERT_NE(b, (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->take_recyclable((EpoxyRegion*)0x10), (EpoxyPage*)nullptr);

  _allocator->add_recyclable(a);
  _allocator->add_recyclable(b);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)2);
  EXPECT_EQ(_allocator->recyclable_pages(), b);

  EpoxyRegion* owner = (EpoxyRegion*)0x10;
  EpoxyPage* first = _allocator->take_recyclable(owner);
  EpoxyPage* second = _allocator->take_recyclable(owner);
  EXPECT_NE(first, second);
  EXPECT_TRUE((first == a && second == b) || (first == b && second == a));
  EXPECT_EQ(first->owner(), owner);
  EXPECT_TRUE(first->is_recycled());
  EXPECT_EQ(first->next(), (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)0);
  EXPECT_EQ(_allocator->take_recyclable(owner), (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->stats()->pages_reused, (size_t)2);
}

// A page goes in the bucket of its biggest hole; a request for N lines gets a
// page whose biggest hole has at least N lines, whatever the order of adding.
TEST_VM_F(EpoxyPageAllocatorTest, take_recyclable_picks_a_page_with_a_big_enough_hole) {
  EpoxyPage* big = _allocator->allocate(1);
  EpoxyPage* small = _allocator->allocate(1);
  ASSERT_NE(big, (EpoxyPage*)nullptr);
  ASSERT_NE(small, (EpoxyPage*)nullptr);
  EpoxyRegion* owner = (EpoxyRegion*)0x10;

  _allocator->add_recyclable(big, 40);
  _allocator->add_recyclable(small, 3);   // added later, but its holes are small
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)2);

  EpoxyPage* got = _allocator->take_recyclable(owner, 20);
  EXPECT_EQ(got, big);
  EXPECT_EQ(got->owner(), owner);
  EXPECT_TRUE(got->is_recycled());
  // Nothing left has a hole of 20 lines.
  EXPECT_EQ(_allocator->take_recyclable(owner, 20), (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)1);
  EXPECT_EQ(_allocator->take_recyclable(owner, 2), small);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)0);
  EXPECT_EQ(_allocator->stats()->pages_reused, (size_t)2);
}

// A small request is given the page with the smallest sufficient hole, so that
// pages with big holes stay for requests that need them.
TEST_VM_F(EpoxyPageAllocatorTest, small_requests_leave_big_holes_alone) {
  EpoxyPage* big = _allocator->allocate(1);
  EpoxyPage* small = _allocator->allocate(1);
  ASSERT_NE(big, (EpoxyPage*)nullptr);
  ASSERT_NE(small, (EpoxyPage*)nullptr);
  EpoxyRegion* owner = (EpoxyRegion*)0x10;

  _allocator->add_recyclable(small, 3);
  _allocator->add_recyclable(big, 40);   // added last
  EXPECT_EQ(_allocator->take_recyclable(owner, 1), small);
  EXPECT_EQ(_allocator->take_recyclable(owner, 1), big);
  EXPECT_EQ(_allocator->take_recyclable(owner, 1), (EpoxyPage*)nullptr);

  // A page whose biggest hole is exactly a power of two serves a request of that many lines.
  EpoxyPage* exact = _allocator->allocate(1);
  ASSERT_NE(exact, (EpoxyPage*)nullptr);
  _allocator->add_recyclable(exact, 16);
  EXPECT_EQ(_allocator->take_recyclable(owner, 16), exact);
}
