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
