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

#include "gc/epoxy/epoxyPageAllocator.hpp"
#include "gc/epoxy/epoxyRegion.hpp"
#include "gc/shared/markBitMap.inline.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/atomicAccess.hpp"
#include "runtime/globals.hpp"
#include "runtime/java.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/copy.hpp"
#include "utilities/powerOfTwo.hpp"

EpoxyPageAllocator::EpoxyPageAllocator(MemRegion reserved, size_t page_size_bytes, bool pre_committed) :
    _reserved(reserved),
    _page_size_bytes(page_size_bytes),
    _page_shift(log2i_exact(page_size_bytes)),
    _max_pages(reserved.byte_size() >> log2i_exact(page_size_bytes)),
    _lines_per_page(page_size_bytes >> EpoxyLineShift),
    _pages(nullptr),
    _lines(nullptr),
    _lines_bytes(0),
    _marks(nullptr),
    _free(reserved.byte_size() >> log2i_exact(page_size_bytes), mtGC),
    _committed(reserved.byte_size() >> log2i_exact(page_size_bytes), mtGC),
    _used_pages(0),
    _committed_bytes(0),
    _recyclable(),
    _recyclable_count(0),
    _lock(Mutex::nosafepoint - 1, "EpoxyPageAllocator_lock") {
  assert(is_aligned(reserved.start(), page_size_bytes), "start must be page aligned");
  assert(is_aligned(reserved.end(), page_size_bytes), "end must be page aligned");

  assert(_max_pages <= UINT_MAX, "page numbers are kept in 32 bits");
  _pages = NEW_C_HEAP_ARRAY(EpoxyPage, _max_pages, mtGC);
  memset(&_stats, 0, sizeof(_stats));

  // One byte per line. The storage is reserved in full and backed by the OS
  // (as zero, which is KEPT) only where it is touched.
  _lines_bytes = align_up(reserved.byte_size() >> EpoxyLineShift, os::vm_allocation_granularity());
  _lines = (uint8_t*)os::reserve_memory(_lines_bytes, mtGC);
  if (_lines == nullptr) {
    vm_exit_during_initialization("Could not reserve memory for the Epoxy line table");
  }
  os::commit_memory_or_exit((char*)_lines, _lines_bytes, false /* executable */, "Epoxy line table");
  for (size_t i = 0; i < _max_pages; i++) {
    EpoxyPage* page = &_pages[i];
    page->_bottom = (HeapWord*)page_base(i);
    page->_end = (HeapWord*)page_base(i + 1);
    page->_top = page->_bottom;
    page->_run_length = 0;
    page->_run_start = (uint)i;
    page->_owner = nullptr;
    page->_next = nullptr;
    page->_promoted = 0;
    page->_recycled = false;
  }
  _free.set_range(0, _max_pages);
  if (pre_committed) {
    _committed.set_range(0, _max_pages);
    _committed_bytes = reserved.byte_size();
  }
}

EpoxyPageAllocator::~EpoxyPageAllocator() {
  FREE_C_HEAP_ARRAY(_pages);
  os::release_memory((char*)_lines, _lines_bytes);
}

void EpoxyPageAllocator::set_lines(size_t first, size_t last, uint8_t state) {
  for (size_t i = first; i < last; i++) {
    AtomicAccess::store(&_lines[i], state);
  }
}

size_t EpoxyPageAllocator::find_free_run(size_t num_pages) const {
  size_t start = _free.find_first_set_bit(0);
  while (start + num_pages <= _max_pages) {
    size_t used = _free.find_first_clear_bit(start, start + num_pages);
    if (used == start + num_pages) {
      return start;
    }
    start = _free.find_first_set_bit(used);
  }
  return _max_pages;
}

bool EpoxyPageAllocator::commit_run(size_t start, size_t num_pages) {
  for (size_t i = start; i < start + num_pages; i++) {
    if (_committed.at(i)) {
      continue;
    }
    if (!os::commit_memory(page_base(i), _page_size_bytes, false /* executable */)) {
      return false;
    }
    if (AlwaysPreTouch) {
      os::pretouch_memory(page_base(i), page_base(i + 1));
    }
    _committed.set_bit(i);
    AtomicAccess::store(&_committed_bytes, _committed_bytes + _page_size_bytes);
  }
  return true;
}

EpoxyPage* EpoxyPageAllocator::allocate(uint num_pages) {
  assert(num_pages > 0, "must ask for at least one page");
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);

  if (num_pages > _max_pages - _used_pages) {
    return nullptr;
  }
  size_t start = find_free_run(num_pages);
  if (start == _max_pages) {
    return nullptr;
  }
  if (!commit_run(start, num_pages)) {
    return nullptr;
  }

  _free.clear_range(start, start + num_pages);
  AtomicAccess::store(&_used_pages, _used_pages + num_pages);

  // page_for() reads this metadata without the lock. Everything a reader
  // uses is written before _run_length, which is what makes the run visible.
  EpoxyPage* first = &_pages[start];
  first->_end = (HeapWord*)page_base(start + num_pages);
  first->_top = first->_bottom;
  first->_owner = nullptr;
  first->_next = nullptr;
  first->_promoted = 0;
  first->_recycled = false;
  for (size_t i = start; i < start + num_pages; i++) {
    AtomicAccess::store(&_pages[i]._run_start, (uint)start);
  }
  AtomicAccess::release_store(&first->_run_length, num_pages);
  return first;
}

void EpoxyPageAllocator::free(EpoxyPage* first) {
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);

  size_t start = pointer_delta(first, _pages, sizeof(EpoxyPage));
  size_t num_pages = first->_run_length;
  assert(start < _max_pages, "not a page of this allocator");
  assert(num_pages > 0, "not the first page of an allocated run");
  assert(!_free.at(start), "double free of page %zu", start);

  // Hide the run from page_for() before anything else changes.
  AtomicAccess::release_store(&first->_run_length, 0u);
  for (size_t i = start; i < start + num_pages; i++) {
    AtomicAccess::store(&_pages[i]._run_start, (uint)i);
  }
  set_lines(start * _lines_per_page, (start + num_pages) * _lines_per_page, EpoxyLineKept);
  // The memory's next user must not inherit promotion marks. A freed run
  // normally has none (it is freed because nothing in it was promoted);
  // clearing costs little and makes that certain.
  if (_marks != nullptr) {
    _marks->clear_range(MemRegion(first->_bottom, first->_end));
  }
#ifdef ASSERT
  // Make any use of a stale reference into this memory fail fast.
  Copy::fill_to_words(first->_bottom, pointer_delta(first->_end, first->_bottom), badHeapWordVal);
#endif
  first->_end = (HeapWord*)page_base(start + 1);
  first->_top = first->_bottom;
  first->_owner = nullptr;
  first->_next = nullptr;
  first->_promoted = 0;
  first->_recycled = false;

  _free.set_range(start, start + num_pages);
  AtomicAccess::store(&_used_pages, _used_pages - num_pages);
}

void EpoxyPageAllocator::set_scratch(EpoxyPage* first, bool scratch) {
  size_t start = pointer_delta(first, _pages, sizeof(EpoxyPage));
  size_t num_pages = first->run_length();
  set_lines(start * _lines_per_page, (start + num_pages) * _lines_per_page,
            scratch ? EpoxyLineScratch : EpoxyLineKept);
}

void EpoxyPageAllocator::mark_lines_promoted(const void* start, const void* end) {
  assert(start < end, "empty range");
  size_t first = line_index(start);
  size_t last = line_index((const char*)end - 1);
  for (size_t i = first; i <= last; i++) {
    assert((AtomicAccess::load(&_lines[i]) & 1) != 0, "line %zu is not a line of a live scratch region", i);
    AtomicAccess::store(&_lines[i], (uint8_t)EpoxyLineScratchPromoted);
  }
}

void EpoxyPageAllocator::release_page(EpoxyPage* page, EpoxyPageRelease* out) {
  size_t first = line_index(page->_bottom);
  size_t last = first + _lines_per_page;
  out->freed = false;
  out->lines_freed = 0;
  out->lines_kept = 0;
  out->max_hole_lines = 0;

  bool has_kept = false;
  for (size_t i = first; i < last && !has_kept; i++) {
    uint8_t v = AtomicAccess::load(&_lines[i]);
    has_kept = (v == EpoxyLineKept || v == EpoxyLineScratchPromoted);
  }
  if (!has_kept) {
    // Nothing promoted and nothing kept from before: the page is freed
    // without being looked at. free() resets the lines and clears the marks.
    out->freed = true;
    out->lines_freed = _lines_per_page;
    free(page);
    return;
  }

  // Lines that became free in an unbroken run are poisoned and have their
  // promotion marks cleared together.
  size_t freed_start = last;   // none
  size_t hole = 0;
  for (size_t i = first; i <= last; i++) {
    uint8_t v = (i < last) ? AtomicAccess::load(&_lines[i]) : (uint8_t)EpoxyLineKept;
    uint8_t now = v;
    if (i < last) {
      if (v == EpoxyLineScratchPromoted) {
        now = EpoxyLineKept;
        AtomicAccess::store(&_lines[i], now);
        out->lines_kept++;
      } else if (v == EpoxyLineScratch) {
        now = EpoxyLineFree;
        AtomicAccess::store(&_lines[i], now);
        out->lines_freed++;
        if (freed_start == last) {
          freed_start = i;
        }
      }
    }
    if (v != EpoxyLineScratch && freed_start != last) {
      HeapWord* from = line_address(freed_start);
      HeapWord* to = line_address(i);
      if (_marks != nullptr) {
        _marks->clear_range(MemRegion(from, to));
      }
#ifdef ASSERT
      Copy::fill_to_words(from, pointer_delta(to, from), badHeapWordVal);
#endif
      freed_start = last;
    }
    if (now == EpoxyLineFree) {
      hole++;
      out->max_hole_lines = MAX2(out->max_hole_lines, hole);
    } else {
      hole = 0;
    }
  }
}

bool EpoxyPageAllocator::find_hole(HeapWord* from, HeapWord* limit, size_t min_lines,
                                   HeapWord** start, HeapWord** end) const {
  assert(min_lines > 0, "a hole has at least one line");
  size_t i = line_index(from);
  size_t last = line_index(limit);
  while (i < last) {
    if (AtomicAccess::load(&_lines[i]) != EpoxyLineFree) {
      i++;
      continue;
    }
    size_t j = i + 1;
    while (j < last && AtomicAccess::load(&_lines[j]) == EpoxyLineFree) {
      j++;
    }
    if (j - i >= min_lines) {
      *start = line_address(i);
      *end = line_address(j);
      return true;
    }
    i = j;
  }
  return false;
}

void EpoxyPageAllocator::take_hole(HeapWord* start, HeapWord* end) {
  size_t first = line_index(start);
  size_t last = line_index(end);
#ifdef ASSERT
  for (size_t i = first; i < last; i++) {
    assert(AtomicAccess::load(&_lines[i]) == EpoxyLineFree, "line %zu is not free", i);
  }
#endif
  set_lines(first, last, EpoxyLineScratch);
}

void EpoxyPageAllocator::add_recyclable(EpoxyPage* page, size_t max_hole_lines) {
  int bucket = MIN2(log2i(MAX2(max_hole_lines, (size_t)1)), RecycleBuckets - 1);
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
  page->_owner = nullptr;
  page->_recycled = false;
  page->_next = _recyclable[bucket];
  _recyclable[bucket] = page;
  _recyclable_count++;
}

EpoxyPage* EpoxyPageAllocator::take_recyclable(EpoxyRegion* owner, size_t min_lines) {
  // Every page of bucket b has a hole of 2^b lines: start at the first bucket
  // that guarantees min_lines, so that small requests leave big holes alone.
  int bucket = log2i_ceil(MAX2(min_lines, (size_t)1));
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
  while (bucket < RecycleBuckets && _recyclable[bucket] == nullptr) {
    bucket++;
  }
  if (bucket >= RecycleBuckets) {
    return nullptr;
  }
  EpoxyPage* page = _recyclable[bucket];
  _recyclable[bucket] = page->_next;
  _recyclable_count--;
  page->_next = nullptr;
  page->_owner = owner;
  page->_recycled = true;
  AtomicAccess::inc(&_stats.pages_reused);
  return page;
}

size_t EpoxyPageAllocator::recyclable_count() const {
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
  return _recyclable_count;
}

size_t EpoxyPageAllocator::free_lines_in_holes_below(size_t min_lines) const {
  size_t total = _max_pages * _lines_per_page;
  size_t small = 0;
  size_t run = 0;
  for (size_t i = 0; i <= total; i++) {
    if (i < total && AtomicAccess::load(&_lines[i]) == EpoxyLineFree) {
      run++;
    } else {
      if (run < min_lines) {
        small += run;
      }
      run = 0;
    }
  }
  return small;
}

EpoxyPage* EpoxyPageAllocator::page_for(const void* addr) const {
  if (!_reserved.contains(addr)) {
    return nullptr;
  }
  size_t index = pointer_delta(addr, _reserved.start(), 1) >> _page_shift;
  // Unlocked read. A caller asking about an address inside a live object holds
  // that object's page allocated, so its entries are stable. For any other
  // address the answer may be stale, but it is always a page that was
  // allocated at some point during the call, or null.
  EpoxyPage* first = &_pages[AtomicAccess::load(&_pages[index]._run_start)];
  return first->run_length() == 0 ? nullptr : first;
}

void EpoxyPageAllocator::log_stats(bool with_small_holes) const {
  if (!log_is_enabled(Info, gc, region)) {
    return;
  }
  log_info(gc, region)("Epoxy regions: %zu released, %zu lines freed, %zu lines kept, %zu pages freed, "
                       "%zu recycled, %zu adopted, %zu recyclable pages taken by regions, "
                       "%zu bytes allocated in recycled holes",
                       AtomicAccess::load(&_stats.regions_released),
                       AtomicAccess::load(&_stats.lines_freed), AtomicAccess::load(&_stats.lines_kept),
                       AtomicAccess::load(&_stats.pages_freed), AtomicAccess::load(&_stats.pages_recycled),
                       AtomicAccess::load(&_stats.pages_adopted), AtomicAccess::load(&_stats.pages_reused),
                       AtomicAccess::load(&_stats.hole_bytes_allocated));
  if (with_small_holes) {
    size_t min_lines = align_up((size_t)MinTLABSize, EpoxyLineSize) / EpoxyLineSize;
    log_info(gc, region)("Epoxy free lines in holes smaller than MinTLABSize (%zu bytes): %zu; "
                         "%zu pages on the recyclable list",
                         (size_t)MinTLABSize, free_lines_in_holes_below(min_lines), recyclable_count());
  }
}

size_t EpoxyPageAllocator::used_pages() const {
  return AtomicAccess::load(&_used_pages);
}

size_t EpoxyPageAllocator::committed_bytes() const {
  return AtomicAccess::load(&_committed_bytes);
}
