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
#include "runtime/atomicAccess.hpp"
#include "threadHelper.inline.hpp"
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

// ---- scratch regions: page kind, promoted counts, release ----

TEST_VM_F(EpoxyRegionTest, scratch_region_marks_its_pages) {
  HeapWord* g = allocate_object(aligned_words(8));
  ASSERT_NE(g, (HeapWord*)nullptr);
  EXPECT_FALSE(_allocator->is_scratch(g));

  EpoxyRegion scratch(_allocator, "scratch", true /* scratch */);
  HeapWord* s = scratch.allocate(aligned_words(8));
  HeapWord* big = scratch.allocate(aligned_words(2 * RegionPageWords + 8));
  ASSERT_NE(s, (HeapWord*)nullptr);
  ASSERT_NE(big, (HeapWord*)nullptr);
  EXPECT_TRUE(scratch.is_scratch());
  EXPECT_TRUE(_allocator->is_scratch(s));
  EXPECT_TRUE(_allocator->is_scratch(big));
  // Every page of a run is scratch, not only the first.
  EXPECT_TRUE(_allocator->is_scratch(big + 2 * RegionPageWords));
  EXPECT_FALSE(_allocator->is_scratch(_base - 1));

  EpoxyPage* kept = nullptr;
  scratch.release(&kept);
}

TEST_VM_F(EpoxyRegionTest, release_frees_pages_without_promoted_objects) {
  EpoxyRegion scratch(_allocator, "scratch", true);
  HeapWord* first = nullptr;
  for (int i = 0; i < 5; i++) {
    HeapWord* p = scratch.allocate(RegionPageWords);
    ASSERT_NE(p, (HeapWord*)nullptr);
    if (first == nullptr) first = p;
  }
  ASSERT_NE(scratch.allocate(aligned_words(3 * RegionPageWords)), (HeapWord*)nullptr);
  EXPECT_EQ(_allocator->used_pages(), (size_t)8);

  EpoxyPage* kept = (EpoxyPage*)0x1;
  scratch.release(&kept);
  EXPECT_EQ(kept, (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->used_pages(), (size_t)0);
  EXPECT_EQ(scratch.used_bytes(), (size_t)0);
  EXPECT_FALSE(_allocator->is_scratch(first));
  EXPECT_EQ(_allocator->page_for(first), (EpoxyPage*)nullptr);

  // The region can be used again after release.
  ASSERT_NE(scratch.allocate(aligned_words(8)), (HeapWord*)nullptr);
  EXPECT_EQ(_allocator->used_pages(), (size_t)1);
  scratch.release(&kept);
}

TEST_VM_F(EpoxyRegionTest, release_keeps_pages_with_promoted_objects) {
  EpoxyRegion scratch(_allocator, "scratch", true);
  HeapWord* a = scratch.allocate(RegionPageWords);
  HeapWord* b = scratch.allocate(RegionPageWords);
  HeapWord* c = scratch.allocate(RegionPageWords);
  size_t run_words = aligned_words(2 * RegionPageWords + 8);
  HeapWord* run = scratch.allocate(run_words);
  ASSERT_NE(run, (HeapWord*)nullptr);
  EXPECT_EQ(_allocator->used_pages(), (size_t)6);

  // Promotion marks the lines of the object. b fills its page, so the page has
  // no free line left and is kept without being recycled.
  _allocator->page_for(b)->note_promoted();
  _allocator->mark_lines_promoted(b, b + RegionPageWords);
  _allocator->page_for(run)->note_promoted();
  _allocator->page_for(run)->note_promoted();
  _allocator->mark_lines_promoted(run, run + run_words);

  EpoxyPage* kept = nullptr;
  scratch.release(&kept);

  // Exactly the page of b and the 3-page run survive, and are no longer scratch.
  EXPECT_EQ(_allocator->used_pages(), (size_t)4);
  int kept_count = 0;
  bool saw_b = false, saw_run = false;
  for (EpoxyPage* p = kept; p != nullptr; p = p->next()) {
    kept_count++;
    saw_b |= (p->bottom() == b);
    saw_run |= (p->bottom() == run);
    EXPECT_EQ(p->owner(), (EpoxyRegion*)nullptr);
    EXPECT_GT(p->promoted_count(), 0u);
  }
  EXPECT_EQ(kept_count, 2);
  EXPECT_TRUE(saw_b);
  EXPECT_TRUE(saw_run);
  EXPECT_FALSE(_allocator->is_scratch(b));
  EXPECT_FALSE(_allocator->is_scratch(run + RegionPageWords));
  EXPECT_EQ((uint)_allocator->line_state(b), (uint)EpoxyLineKept);
  EXPECT_EQ((uint)_allocator->line_state(run + RegionPageWords), (uint)EpoxyLineKept);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)0);
  EXPECT_EQ(_allocator->page_for(a), (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->page_for(c), (EpoxyPage*)nullptr);
  EXPECT_NE(_allocator->page_for(b), (EpoxyPage*)nullptr);
}

// ---- reuse of the free lines of kept pages ----

static const size_t LineWords = EpoxyLineSize / HeapWordSize;
static const size_t LinesPerPage = RegionPageSize / EpoxyLineSize;

class EpoxyLineReuseTest : public EpoxyRegionTest {
protected:
  uint state(const void* addr) const { return (uint)_allocator->line_state(addr); }
  static HeapWord* line_start(EpoxyPage* page, size_t line) { return page->bottom() + line * LineWords; }
  static size_t line_of(EpoxyPage* page, const void* addr) {
    return pointer_delta(addr, page->bottom(), 1) / EpoxyLineSize;
  }

  // Allocates and formats one object in a scratch region.
  static HeapWord* alloc_obj(EpoxyRegion* region, size_t words) {
    HeapWord* p = region->allocate(words);
    if (p != nullptr) {
      CollectedHeap::fill_with_object(p, words);
    }
    return p;
  }

  // One scratch region fills a fresh page with one-line objects, promotes the
  // objects on the given lines and is released: the page keeps exactly those
  // lines. Returns the page; *adopted gets what release handed back.
  EpoxyPage* make_kept_page(const size_t* lines, size_t count, EpoxyPage** adopted = nullptr) {
    EpoxyRegion maker(_allocator, "maker", true);
    // One request of a whole page: too big for a recycled hole, so the page is fresh.
    HeapWord* first = maker.allocate(RegionPageWords);
    EXPECT_NE(first, (HeapWord*)nullptr);
    if (first == nullptr) {
      return nullptr;
    }
    EpoxyPage* page = _allocator->page_for(first);
    EXPECT_EQ(maker.used_bytes(), RegionPageSize);   // exactly one page
    for (size_t i = 0; i < count; i++) {
      _allocator->page_for(first)->note_promoted();
      _allocator->mark_lines_promoted(line_start(page, lines[i]), line_start(page, lines[i] + 1));
    }
    EpoxyPage* kept = nullptr;
    maker.release(&kept);
    if (adopted != nullptr) {
      *adopted = kept;
    }
    return page;
  }

  // All free-line states of the page, 2 or 0, as a string for failure messages.
  void expect_only_kept_lines(EpoxyPage* page, const size_t* kept_lines, size_t count) {
    for (size_t l = 0; l < LinesPerPage; l++) {
      bool kept = false;
      for (size_t i = 0; i < count; i++) kept |= (kept_lines[i] == l);
      EXPECT_EQ(state(line_start(page, l)), kept ? (uint)EpoxyLineKept : (uint)EpoxyLineFree) << "line " << l;
    }
  }
};

TEST_VM_F(EpoxyLineReuseTest, release_with_scattered_promoted_objects_sets_line_states) {
  EpoxyRegion scratch(_allocator, "scratch", true);
  HeapWord* objs[100];
  for (int i = 0; i < 100; i++) {
    objs[i] = scratch.allocate(16);     // 128 bytes: two to a line
    ASSERT_NE(objs[i], (HeapWord*)nullptr);
  }
  EpoxyPage* page = _allocator->page_for(objs[0]);
  ASSERT_NE(page, (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->used_pages(), (size_t)1);

  // Objects 3, 10, 11 and 50 are promoted: lines 1, 5 (both of 10 and 11) and 25.
  int promoted[] = {3, 10, 11, 50};
  for (int i : promoted) {
    page->note_promoted();
    _allocator->mark_lines_promoted(objs[i], objs[i] + 16);
  }
  EXPECT_EQ(state(objs[3]), (uint)EpoxyLineScratchPromoted);
  EXPECT_EQ(state(objs[0]), (uint)EpoxyLineScratch);
  EXPECT_EQ(state(line_start(page, 200)), (uint)EpoxyLineScratch);   // taken with the page, not yet used
  EXPECT_TRUE(_allocator->is_scratch(objs[3]));
  EXPECT_TRUE(_allocator->is_scratch(objs[0]));

  EpoxyPage* kept = (EpoxyPage*)0x1;
  scratch.release(&kept);

  // The page keeps three lines, so it is not freed; its holes are big enough
  // for the recyclable list, not the adopted list.
  EXPECT_EQ(kept, (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->used_pages(), (size_t)1);
  EXPECT_EQ(_allocator->recyclable_pages(), page);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)1);
  EXPECT_EQ(page->owner(), (EpoxyRegion*)nullptr);
  EXPECT_FALSE(page->is_recycled());
  EXPECT_EQ(page->top(), page->end());
  size_t kept_lines[] = {1, 5, 25};
  expect_only_kept_lines(page, kept_lines, 3);
  for (int i : promoted) {
    EXPECT_FALSE(_allocator->is_scratch(objs[i]));
  }
  EXPECT_FALSE(_allocator->is_scratch(objs[0]));
  EXPECT_EQ(_allocator->stats()->lines_kept, (size_t)3);
  EXPECT_EQ(_allocator->stats()->lines_freed, LinesPerPage - 3);
  EXPECT_EQ(_allocator->stats()->pages_recycled, (size_t)1);
  EXPECT_EQ(_allocator->stats()->pages_freed, (size_t)0);
#ifdef ASSERT
  // Freed lines are poisoned, kept ones are not touched.
  EXPECT_EQ(*(uintptr_t*)line_start(page, 0), (((uintptr_t)badHeapWordVal << 32) | (uintptr_t)badHeapWordVal));
  EXPECT_EQ(*(uintptr_t*)line_start(page, 200), (((uintptr_t)badHeapWordVal << 32) | (uintptr_t)badHeapWordVal));
#endif
}

TEST_VM_F(EpoxyLineReuseTest, a_hole_is_reused_by_a_second_region_and_freed_again) {
  size_t kept_lines[] = {10, 20};
  EpoxyPage* page = make_kept_page(kept_lines, 2);
  ASSERT_NE(page, (EpoxyPage*)nullptr);
  expect_only_kept_lines(page, kept_lines, 2);
  ASSERT_EQ(_allocator->recyclable_count(), (size_t)1);

  EpoxyRegion second(_allocator, "second", true);
  const size_t HoleObjects = (LinesPerPage - 2) * 2;   // 16-word objects, two to a line
  HeapWord* first = nullptr;
  for (size_t i = 0; i < HoleObjects; i++) {
    HeapWord* p = alloc_obj(&second, 16);
    ASSERT_NE(p, (HeapWord*)nullptr);
    if (first == nullptr) first = p;
    ASSERT_EQ(_allocator->page_for(p), page) << "object " << i;
    size_t line = line_of(page, p);
    ASSERT_NE(line, (size_t)10);
    ASSERT_NE(line, (size_t)20);
    ASSERT_EQ(state(p), (uint)EpoxyLineScratch);
    ASSERT_TRUE(_allocator->is_scratch(p));
    ASSERT_EQ(page->owner(), &second);
  }
  EXPECT_EQ(first, page->bottom());
  EXPECT_TRUE(page->is_recycled());
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)0);
  EXPECT_EQ(_allocator->stats()->pages_reused, (size_t)1);
  // The kept lines are still not scratch.
  EXPECT_EQ(state(line_start(page, 10)), (uint)EpoxyLineKept);
  EXPECT_EQ(state(line_start(page, 20)), (uint)EpoxyLineKept);

  // Three holes were used, each recorded as a span that is walkable.
  ASSERT_EQ(second.span_count(), 3);
  EXPECT_EQ(second.span_at(0).start, line_start(page, 0));
  EXPECT_EQ(second.span_at(0).top, line_start(page, 10));
  EXPECT_EQ(second.span_at(1).start, line_start(page, 11));
  EXPECT_EQ(second.span_at(1).top, line_start(page, 20));
  EXPECT_EQ(second.span_at(2).start, line_start(page, 21));
  EXPECT_EQ(second.span_at(2).top, line_start(page, LinesPerPage));

  // The page is full: the next object comes from a fresh page.
  HeapWord* extra = alloc_obj(&second, 16);
  ASSERT_NE(extra, (HeapWord*)nullptr);
  EpoxyPage* fresh = _allocator->page_for(extra);
  EXPECT_NE(fresh, page);
  EXPECT_FALSE(fresh->is_recycled());
  EXPECT_EQ(_allocator->used_pages(), (size_t)2);

  // The walk sees every object of the region once and nothing in the kept lines.
  RecordingClosure cl;
  second.object_iterate(&cl);
  ASSERT_EQ(cl._starts.length(), (int)HoleObjects + 1);
  for (int i = 0; i < cl._starts.length(); i++) {
    EXPECT_NE(cl._starts.at(i), line_start(page, 10));
    EXPECT_NE(cl._starts.at(i), line_start(page, 20));
    EXPECT_EQ(cl._sizes.at(i), (size_t)16);
  }

  EpoxyPage* kept = (EpoxyPage*)0x1;
  second.release(&kept);
  EXPECT_EQ(kept, (EpoxyPage*)nullptr);
  // The fresh page is freed; the old one has its two kept lines and is
  // recyclable again, its other lines free.
  EXPECT_EQ(_allocator->used_pages(), (size_t)1);
  EXPECT_EQ(_allocator->recyclable_pages(), page);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)1);
  EXPECT_FALSE(page->is_recycled());
  EXPECT_EQ(page->owner(), (EpoxyRegion*)nullptr);
  expect_only_kept_lines(page, kept_lines, 2);
  EXPECT_FALSE(_allocator->is_scratch(first));
  EXPECT_EQ(_allocator->page_for(extra), (EpoxyPage*)nullptr);
#ifdef ASSERT
  EXPECT_EQ(*(uintptr_t*)line_start(page, 11), (((uintptr_t)badHeapWordVal << 32) | (uintptr_t)badHeapWordVal));
#endif
}

TEST_VM_F(EpoxyLineReuseTest, an_object_that_spans_lines_keeps_every_line) {
  EpoxyRegion scratch(_allocator, "scratch", true);
  HeapWord* small = scratch.allocate(20);     // lines 0 (words 0 to 19)
  HeapWord* big = scratch.allocate(100);      // words 20 to 119: lines 0 to 3
  HeapWord* after = scratch.allocate(20);     // words 120 to 139: lines 3 and 4
  ASSERT_NE(small, (HeapWord*)nullptr);
  ASSERT_NE(big, (HeapWord*)nullptr);
  ASSERT_NE(after, (HeapWord*)nullptr);
  EpoxyPage* page = _allocator->page_for(small);
  EXPECT_EQ(line_of(page, big), (size_t)0);
  EXPECT_EQ(line_of(page, big + 99), (size_t)3);

  page->note_promoted();
  _allocator->mark_lines_promoted(big, big + 100);
  for (size_t l = 0; l <= 3; l++) {
    EXPECT_EQ(state(line_start(page, l)), (uint)EpoxyLineScratchPromoted) << "line " << l;
  }
  EXPECT_EQ(state(line_start(page, 4)), (uint)EpoxyLineScratch);

  EpoxyPage* kept = nullptr;
  scratch.release(&kept);
  EXPECT_EQ(_allocator->recyclable_pages(), page);
  size_t kept_lines[] = {0, 1, 2, 3};
  expect_only_kept_lines(page, kept_lines, 4);
  EXPECT_FALSE(_allocator->is_scratch(big));
  EXPECT_FALSE(_allocator->is_scratch(big + 99));

  // Nothing is ever allocated over the object: the free lines start at line 4.
  EpoxyRegion second(_allocator, "second", true);
  HeapWord* p = second.allocate(16);
  ASSERT_NE(p, (HeapWord*)nullptr);
  EXPECT_EQ(p, line_start(page, 4));
  for (int i = 0; i < 100; i++) {
    HeapWord* q = second.allocate(16);
    ASSERT_NE(q, (HeapWord*)nullptr);
    EXPECT_GE(q, line_start(page, 4));
  }
  second.release(&kept);
}

TEST_VM_F(EpoxyLineReuseTest, a_promoted_object_in_a_recycled_hole_survives_a_second_release) {
  size_t kept_lines[] = {10, 20};
  EpoxyPage* page = make_kept_page(kept_lines, 2);
  ASSERT_NE(page, (EpoxyPage*)nullptr);

  EpoxyRegion second(_allocator, "second", true);
  HeapWord* objs[40];
  for (int i = 0; i < 40; i++) {
    objs[i] = alloc_obj(&second, 16);
    ASSERT_NE(objs[i], (HeapWord*)nullptr);
    ASSERT_EQ(_allocator->page_for(objs[i]), page);
  }
  EXPECT_EQ(line_of(page, objs[4]), (size_t)2);
  EXPECT_EQ(line_of(page, objs[25]), (size_t)13);   // 20 objects in lines 0 to 9, then lines 11 on
  page->note_promoted();
  _allocator->mark_lines_promoted(objs[4], objs[4] + 16);
  page->note_promoted();
  _allocator->mark_lines_promoted(objs[25], objs[25] + 16);

  EpoxyPage* kept = (EpoxyPage*)0x1;
  second.release(&kept);
  EXPECT_EQ(kept, (EpoxyPage*)nullptr);
  size_t now_kept[] = {2, 10, 13, 20};
  expect_only_kept_lines(page, now_kept, 4);
  EXPECT_EQ(_allocator->recyclable_pages(), page);
  EXPECT_FALSE(_allocator->is_scratch(objs[4]));
  EXPECT_FALSE(_allocator->is_scratch(objs[25]));
  EXPECT_EQ(_allocator->stats()->lines_kept, (size_t)2 + 2);   // the first release kept two lines too

  // A third region allocates in every remaining free line, and in no other.
  EpoxyRegion third(_allocator, "third", true);
  const size_t HoleObjects = (LinesPerPage - 4) * 2;
  for (size_t i = 0; i < HoleObjects; i++) {
    HeapWord* p = alloc_obj(&third, 16);
    ASSERT_NE(p, (HeapWord*)nullptr);
    ASSERT_EQ(_allocator->page_for(p), page) << "object " << i;
    size_t line = line_of(page, p);
    ASSERT_TRUE(line != 2 && line != 10 && line != 13 && line != 20) << "object " << i << " in line " << line;
  }
  HeapWord* extra = alloc_obj(&third, 16);
  ASSERT_NE(extra, (HeapWord*)nullptr);
  EXPECT_NE(_allocator->page_for(extra), page);
  third.release(&kept);
  EXPECT_EQ(kept, (EpoxyPage*)nullptr);
  expect_only_kept_lines(page, now_kept, 4);
  EXPECT_EQ(_allocator->recyclable_pages(), page);
  EXPECT_EQ(_allocator->used_pages(), (size_t)1);
}

// A page goes on the recyclable list only if some hole has EpoxyMinRecycleLines lines.
TEST_VM_F(EpoxyLineReuseTest, hole_size_threshold_decides_recyclable_or_adopted) {
  const size_t Min = EpoxyMinRecycleLines;
  ASSERT_GE(Min, (size_t)2);
  ASSERT_LT(Min * 4, LinesPerPage);

  // Kept lines every Min lines, and the last line: the holes are Min - 1 lines.
  size_t small[LinesPerPage];
  size_t small_count = 0;
  for (size_t l = 0; l < LinesPerPage; l += Min) small[small_count++] = l;
  EpoxyPage* adopted = nullptr;
  EpoxyPage* small_holes = make_kept_page(small, small_count, &adopted);
  ASSERT_NE(small_holes, (EpoxyPage*)nullptr);
  EXPECT_EQ(adopted, small_holes);
  EXPECT_EQ(adopted->next(), (EpoxyPage*)nullptr);
  EXPECT_EQ(adopted->owner(), (EpoxyRegion*)nullptr);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)0);
  EXPECT_EQ(_allocator->stats()->pages_adopted, (size_t)1);
  EXPECT_EQ(_allocator->stats()->pages_recycled, (size_t)0);
  // 16 or so free lines in holes of fewer than Min lines.
  EXPECT_GT(_allocator->free_lines_in_holes_below(Min), (size_t)0);
  EXPECT_EQ(_allocator->free_lines_in_holes_below(Min - 1), (size_t)0);

  // The same with holes of exactly Min lines.
  size_t exact[LinesPerPage];
  size_t exact_count = 0;
  for (size_t l = 0; l < LinesPerPage; l += Min + 1) exact[exact_count++] = l;
  if (exact[exact_count - 1] != LinesPerPage - 1) exact[exact_count++] = LinesPerPage - 1;
  EpoxyPage* none = (EpoxyPage*)0x1;
  EpoxyPage* exact_holes = make_kept_page(exact, exact_count, &none);
  ASSERT_NE(exact_holes, (EpoxyPage*)nullptr);
  EXPECT_EQ(none, (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->recyclable_pages(), exact_holes);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)1);

  // A region only ever allocates in the recyclable one.
  EpoxyRegion r(_allocator, "r", true);
  HeapWord* p = r.allocate(16);
  ASSERT_NE(p, (HeapWord*)nullptr);
  EXPECT_EQ(_allocator->page_for(p), exact_holes);
  EpoxyPage* kept = nullptr;
  r.release(&kept);
}

// Requests above EpoxyMaxRecycleBytes go to fresh pages even if a recycled page is available.
TEST_VM_F(EpoxyLineReuseTest, large_requests_bypass_recycled_pages) {
  size_t kept_lines[] = {100};
  EpoxyPage* page = make_kept_page(kept_lines, 1);
  ASSERT_NE(page, (EpoxyPage*)nullptr);
  ASSERT_EQ(_allocator->recyclable_count(), (size_t)1);

  const size_t MaxWords = MIN2((size_t)EpoxyMaxRecycleBytes, (size_t)EpoxyMinRecycleLines * EpoxyLineSize) / HeapWordSize;
  EpoxyRegion r(_allocator, "r", true);
  HeapWord* big = r.allocate(MaxWords + 2);
  ASSERT_NE(big, (HeapWord*)nullptr);
  EXPECT_NE(_allocator->page_for(big), page);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)1);   // untouched
  EXPECT_EQ(page->owner(), (EpoxyRegion*)nullptr);

  // A ranged request whose minimum is too big goes to a fresh page as well.
  size_t actual = 0;
  HeapWord* tlab = r.allocate(MaxWords + 2, MaxWords + 100, &actual);
  ASSERT_NE(tlab, (HeapWord*)nullptr);
  EXPECT_NE(_allocator->page_for(tlab), page);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)1);

  // A request of exactly the limit is served from the hole.
  // (By a region with no fresh page yet: a fresh page with room is used first.)
  EpoxyRegion r2(_allocator, "r2", true);
  HeapWord* limit = r2.allocate(MaxWords);
  ASSERT_NE(limit, (HeapWord*)nullptr);
  EXPECT_EQ(_allocator->page_for(limit), page);
  EXPECT_EQ(page->owner(), &r2);
  EXPECT_EQ(limit, page->bottom());

  // A request bigger than a page is a run, as ever.
  HeapWord* run = r.allocate(aligned_words(2 * RegionPageWords + 8));
  ASSERT_NE(run, (HeapWord*)nullptr);
  EXPECT_EQ(_allocator->page_for(run)->run_length(), 3u);

  EpoxyPage* kept = nullptr;
  r.release(&kept);
  r2.release(&kept);
  EXPECT_EQ(kept, (EpoxyPage*)nullptr);
  EXPECT_EQ(_allocator->used_pages(), (size_t)1);
  EXPECT_EQ(_allocator->recyclable_pages(), page);
}

// A TLAB-style request takes a hole when the hole is at least the minimum, and
// what it gets lies entirely inside that hole.
TEST_VM_F(EpoxyLineReuseTest, ranged_request_in_a_hole_stays_inside_it) {
  // Lines 0 to 2 are a hole of three lines (96 words); line 3 is kept; the rest is free.
  size_t kept_lines[] = {3};
  EpoxyPage* page = make_kept_page(kept_lines, 1);
  ASSERT_NE(page, (EpoxyPage*)nullptr);

  EpoxyRegion r(_allocator, "r", true);
  size_t actual = 0;
  HeapWord* a = r.allocate(64, 400, &actual);   // fits the first hole: it gets all of it
  ASSERT_NE(a, (HeapWord*)nullptr);
  EXPECT_EQ(a, line_start(page, 0));
  EXPECT_EQ(actual, 3 * LineWords);
  EXPECT_EQ(state(a), (uint)EpoxyLineScratch);

  // The next one needs more than the first hole had: it moves to the hole after line 3.
  HeapWord* b = r.allocate(200, 300, &actual);
  ASSERT_NE(b, (HeapWord*)nullptr);
  EXPECT_EQ(b, line_start(page, 4));
  EXPECT_EQ(actual, (size_t)300);
  EXPECT_EQ(_allocator->page_for(b), page);

  // A second request with a small minimum takes the rest of that hole, in pieces.
  HeapWord* c = r.allocate(64, 100, &actual);
  ASSERT_NE(c, (HeapWord*)nullptr);
  EXPECT_EQ(c, b + 300);
  EXPECT_EQ(actual, (size_t)100);
  EpoxyPage* kept = nullptr;
  r.release(&kept);

  // And a region whose first minimum is below the first hole gets exactly that hole.
  EpoxyRegion r2(_allocator, "r2", true);
  HeapWord* d = r2.allocate(10, 30, &actual);
  ASSERT_NE(d, (HeapWord*)nullptr);
  EXPECT_EQ(d, line_start(page, 0));
  EXPECT_EQ(actual, (size_t)30);
  HeapWord* e = r2.allocate(10, 400, &actual);   // only 66 words are left of that hole
  ASSERT_NE(e, (HeapWord*)nullptr);
  EXPECT_EQ(e, d + 30);
  EXPECT_EQ(actual, 3 * LineWords - 30);
  EXPECT_LE(e + actual, line_start(page, 3));
  r2.release(&kept);
}

// Many threads, each with its own scratch region, take recycled pages from the
// shared list: no page is taken twice, no memory is handed out twice, no page is lost.
TEST_VM_F(EpoxyLineReuseTest, concurrent_regions_share_the_recyclable_list) {
  const int KeptPages = 16;
  size_t kept_lines[] = {0};
  for (int i = 0; i < KeptPages; i++) {
    ASSERT_NE(make_kept_page(kept_lines, 1), (EpoxyPage*)nullptr);
  }
  ASSERT_EQ(_allocator->recyclable_count(), (size_t)KeptPages);

  const int Threads = 4;
  const int Objects = 1500;
  EpoxyPageAllocator* allocator = _allocator;
  volatile int failures = 0;
  volatile int* failures_addr = &failures;

  auto worker = [allocator, failures_addr](Thread* thread, int id) {
    EpoxyRegion region(allocator, "worker", true);
    HeapWord** objs = NEW_C_HEAP_ARRAY(HeapWord*, Objects, mtTest);
    for (int i = 0; i < Objects; i++) {
      HeapWord* p = region.allocate(16);
      if (p == nullptr) {
        AtomicAccess::inc(failures_addr);
        objs[i] = nullptr;
        continue;
      }
      objs[i] = p;
      for (int w = 0; w < 16; w++) {
        ((intptr_t*)p)[w] = ((intptr_t)id << 32) | i;
      }
      EpoxyPage* page = allocator->page_for(p);
      if (page == nullptr || page->owner() != &region || !allocator->is_scratch(p)) {
        AtomicAccess::inc(failures_addr);
      }
    }
    os::naked_yield();
    for (int i = 0; i < Objects; i++) {
      if (objs[i] == nullptr) continue;
      for (int w = 0; w < 16; w++) {
        if (((intptr_t*)objs[i])[w] != (((intptr_t)id << 32) | i)) {
          AtomicAccess::inc(failures_addr);
        }
      }
    }
    FREE_C_HEAP_ARRAY(objs);
    EpoxyPage* kept = nullptr;
    region.release(&kept);
    if (kept != nullptr) {
      AtomicAccess::inc(failures_addr);
    }
  };

  TestThreadGroup<decltype(worker)> group(worker, Threads);
  group.doit();
  group.join();

  EXPECT_EQ(failures, 0);
  // Every page is back on the list exactly once, and none is in use otherwise.
  EXPECT_EQ(_allocator->used_pages(), (size_t)KeptPages);
  EXPECT_EQ(_allocator->recyclable_count(), (size_t)KeptPages);
  int listed = 0;
  for (EpoxyPage* p = _allocator->recyclable_pages(); p != nullptr; p = p->next()) {
    listed++;
    EXPECT_EQ(p->owner(), (EpoxyRegion*)nullptr);
    EXPECT_FALSE(p->is_recycled());
    EXPECT_LE(listed, KeptPages);
  }
  EXPECT_EQ(listed, KeptPages);
}
