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
#include "gc/shared/collectedHeap.hpp"
#include "logging/log.hpp"
#include "memory/iterator.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomicAccess.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/align.hpp"

EpoxyRegion::EpoxyRegion(EpoxyPageAllocator* pages, const char* name, bool scratch) :
    _pages(pages),
    _name(name),
    _page_words(pages->page_size_bytes() / HeapWordSize),
    _scratch(scratch),
    _parent(nullptr),
    _depth(0),
    _scope_frame(nullptr),
    _current(nullptr),
    _first(nullptr),
    _last(nullptr),
    _owned_pages(0),
    _lock(Mutex::nosafepoint, "EpoxyRegion_lock") {
}

void EpoxyRegion::append(EpoxyPage* page) {
  page->set_owner(this);
  page->set_next(nullptr);
  if (_scratch) {
    _pages->set_scratch(page, true);
  }
  if (_last == nullptr) {
    _first = page;
  } else {
    _last->set_next(page);
  }
  _last = page;
  AtomicAccess::store(&_owned_pages, _owned_pages + page->run_length());
}

HeapWord* EpoxyRegion::allocate_run(size_t size_in_words) {
  size_t num_pages = align_up(size_in_words, _page_words) / _page_words;
  if (num_pages > _pages->max_pages()) {
    return nullptr;
  }

  EpoxyPage* run = _pages->allocate((uint)num_pages);
  if (run == nullptr) {
    return nullptr;
  }
  // The run holds exactly one object. What is left of its last page stays
  // unused: top() marks where the object ends.
  HeapWord* result = run->bottom();
  run->set_top(result + size_in_words);

  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
  append(run);
  return result;
}

HeapWord* EpoxyRegion::allocate(size_t size_in_words) {
  assert(is_object_aligned(size_in_words), "size must be object aligned: %zu", size_in_words);

  if (size_in_words > _page_words) {
    return allocate_run(size_in_words);
  }

  while (true) {
    EpoxyPage* page = AtomicAccess::load_acquire(&_current);
    if (page != nullptr) {
      HeapWord* top = AtomicAccess::load(page->top_addr());
      if (size_in_words <= pointer_delta(page->end(), top)) {
        if (AtomicAccess::cmpxchg(page->top_addr(), top, top + size_in_words) == top) {
          return top;
        }
        continue;  // lost a race for this page; try again
      }
    }

    // The current page cannot take this request: serve it from a new page.
    // Every page keeps its top(), so it stays walkable. This holds only while
    // pages are never taken away from a region: a thread that still holds a
    // pointer to the old page may bump it a little further.
    MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
    if (AtomicAccess::load(&_current) != page) {
      continue;  // another thread already moved on
    }
    EpoxyPage* fresh = _pages->allocate(1);
    if (fresh == nullptr) {
      // Leave the current page in place: a smaller request may still fit in it.
      return nullptr;
    }
    // The fresh page is not visible to other threads yet.
    HeapWord* result = fresh->bottom();
    fresh->set_top(result + size_in_words);
    append(fresh);

    // Keep bumping whichever page has more room; the other one's tail is left
    // behind. Without this a run of mid-sized objects wastes half the heap.
    size_t fresh_free = pointer_delta(fresh->end(), fresh->top());
    size_t old_free = (page == nullptr) ? 0 : pointer_delta(page->end(), AtomicAccess::load(page->top_addr()));
    if (page == nullptr || fresh_free > old_free) {
      AtomicAccess::release_store(&_current, fresh);
    }
    return result;
  }
}

HeapWord* EpoxyRegion::allocate(size_t min_words, size_t desired_words, size_t* actual_words) {
  assert(min_words <= desired_words, "min %zu must not exceed desired %zu", min_words, desired_words);
  assert(desired_words <= _page_words, "a ranged request must fit in one page: %zu", desired_words);

  while (true) {
    EpoxyPage* page = AtomicAccess::load_acquire(&_current);
    if (page != nullptr) {
      HeapWord* top = AtomicAccess::load(page->top_addr());
      size_t free_words = align_down(pointer_delta(page->end(), top), (size_t)MinObjAlignment);
      if (free_words >= min_words) {
        size_t words = MIN2(free_words, desired_words);
        if (AtomicAccess::cmpxchg(page->top_addr(), top, top + words) == top) {
          *actual_words = words;
          return top;
        }
        continue;  // lost a race for this page; try again
      }
    }

    MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
    if (AtomicAccess::load(&_current) != page) {
      continue;  // another thread already moved on
    }
    EpoxyPage* fresh = _pages->allocate(1);
    if (fresh == nullptr) {
      return nullptr;
    }
    append(fresh);
    AtomicAccess::release_store(&_current, fresh);
  }
}

void EpoxyRegion::object_iterate(ObjectClosure* cl) {
  for (EpoxyPage* page = _first; page != nullptr; page = page->next()) {
    HeapWord* p = page->bottom();
    HeapWord* top = page->top();
    while (p < top) {
      oop obj = cast_to_oop(p);
      size_t size = obj->size();
      cl->do_object(obj);
      p += size;
    }
    assert(p == top, "page must be walkable to exactly its top");
  }
}

void EpoxyRegion::release(EpoxyPage** kept) {
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);

  EpoxyPage* kept_list = nullptr;
  size_t freed_pages = 0;
  size_t kept_pages = 0;
  size_t promoted_objects = 0;
  EpoxyPage* page = _first;
  while (page != nullptr) {
    EpoxyPage* next = page->next();
    if (page->promoted_count() == 0) {
      freed_pages += page->run_length();
      _pages->free(page);
    } else {
      kept_pages += page->run_length();
      promoted_objects += page->promoted_count();
      _pages->set_scratch(page, false);
      page->set_owner(nullptr);
      page->set_next(kept_list);
      kept_list = page;
    }
    page = next;
  }

  AtomicAccess::release_store(&_current, (EpoxyPage*)nullptr);
  _first = nullptr;
  _last = nullptr;
  AtomicAccess::store(&_owned_pages, (size_t)0);
  *kept = kept_list;
  log_debug(gc, region)("Epoxy region released: %zu pages freed, %zu kept for %zu promoted objects",
                        freed_pages, kept_pages, promoted_objects);
}

size_t EpoxyRegion::used_bytes() const {
  return AtomicAccess::load(&_owned_pages) * _pages->page_size_bytes();
}
