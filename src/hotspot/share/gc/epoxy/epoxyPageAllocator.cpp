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
#include "gc/shared/markBitMap.inline.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/atomicAccess.hpp"
#include "runtime/globals.hpp"
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
    _pages(nullptr),
    _kind(nullptr),
    _marks(nullptr),
    _free(reserved.byte_size() >> log2i_exact(page_size_bytes), mtGC),
    _committed(reserved.byte_size() >> log2i_exact(page_size_bytes), mtGC),
    _used_pages(0),
    _committed_bytes(0),
    _lock(Mutex::nosafepoint - 1, "EpoxyPageAllocator_lock") {
  assert(is_aligned(reserved.start(), page_size_bytes), "start must be page aligned");
  assert(is_aligned(reserved.end(), page_size_bytes), "end must be page aligned");

  assert(_max_pages <= UINT_MAX, "page numbers are kept in 32 bits");
  _pages = NEW_C_HEAP_ARRAY(EpoxyPage, _max_pages, mtGC);
  _kind = NEW_C_HEAP_ARRAY(uint8_t, _max_pages, mtGC);
  memset(_kind, 0, _max_pages);
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
  }
  _free.set_range(0, _max_pages);
  if (pre_committed) {
    _committed.set_range(0, _max_pages);
    _committed_bytes = reserved.byte_size();
  }
}

EpoxyPageAllocator::~EpoxyPageAllocator() {
  FREE_C_HEAP_ARRAY(_pages);
  FREE_C_HEAP_ARRAY(_kind);
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
    _kind[i] = 0;
  }
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

  _free.set_range(start, start + num_pages);
  AtomicAccess::store(&_used_pages, _used_pages - num_pages);
}

void EpoxyPageAllocator::set_scratch(EpoxyPage* first, bool scratch) {
  size_t start = pointer_delta(first, _pages, sizeof(EpoxyPage));
  size_t num_pages = first->run_length();
  for (size_t i = start; i < start + num_pages; i++) {
    AtomicAccess::store(&_kind[i], (uint8_t)(scratch ? 1 : 0));
  }
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

size_t EpoxyPageAllocator::used_pages() const {
  return AtomicAccess::load(&_used_pages);
}

size_t EpoxyPageAllocator::committed_bytes() const {
  return AtomicAccess::load(&_committed_bytes);
}
