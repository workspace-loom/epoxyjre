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
#include "gc/shared/gc_globals.hpp"
#include "logging/log.hpp"
#include "memory/iterator.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomicAccess.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/align.hpp"

// Every this many released scratch regions the totals are logged (info), so
// that a VM that is killed rather than shut down still leaves a record.
static const size_t StatsLogInterval = 4096;

EpoxyRegion::EpoxyRegion(EpoxyPageAllocator* pages, const char* name, bool scratch) :
    _pages(pages),
    _name(name),
    _page_words(pages->page_size_bytes() / HeapWordSize),
    // Bigger requests get a fresh page: a hole that size is rare, and a quarter
    // of a page is the most a hole search is worth.
    _max_recycle_words(MIN2((size_t)EpoxyMaxRecycleBytes, pages->page_size_bytes() / 4) / HeapWordSize),
    _scratch(scratch),
    _parent(nullptr),
    _depth(0),
    _scope_frame(nullptr),
    _current(nullptr),
    _first(nullptr),
    _last(nullptr),
    _owned_pages(0),
    _lock(Mutex::nosafepoint, "EpoxyRegion_lock"),
    _hole_top(nullptr),
    _hole_end(nullptr),
    _hole_span(-1),
    _recycled(nullptr),
    _cursor(nullptr),
    _spans() {
}

void EpoxyRegion::link(EpoxyPage* page) {
  page->set_next(nullptr);
  if (_last == nullptr) {
    _first = page;
  } else {
    _last->set_next(page);
  }
  _last = page;
  AtomicAccess::store(&_owned_pages, _owned_pages + page->run_length());
}

// A fresh page or run: all its lines become scratch.
void EpoxyRegion::append(EpoxyPage* page) {
  page->set_owner(this);
  if (_scratch) {
    _pages->set_scratch(page, true);
    _spans.push(Span{page->bottom(), nullptr, page});
  }
  link(page);
}

EpoxySpan EpoxyRegion::span_at(int index) const {
  const Span& span = _spans.at(index);
  HeapWord* top = span.top;
  if (span.page != nullptr) {
    top = span.page->top();
  } else if (index == _hole_span) {
    top = _hole_top;
  }
  return EpoxySpan{span.start, top};
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

// Bumps in the current hole. Only the owning thread of a scratch region gets here.
HeapWord* EpoxyRegion::bump_hole(size_t min_words, size_t desired_words, size_t* actual_words) {
  while (true) {
    HeapWord* top = AtomicAccess::load(&_hole_top);
    size_t free_words = align_down(pointer_delta(_hole_end, top), (size_t)MinObjAlignment);
    if (free_words < min_words) {
      return nullptr;
    }
    size_t words = MIN2(free_words, desired_words);
    if (AtomicAccess::cmpxchg(&_hole_top, top, top + words) == top) {
      *actual_words = words;
      return top;
    }
  }
}

// Ends the current hole: its span is final and what is left of it is abandoned.
void EpoxyRegion::close_hole() {
  if (_hole_span >= 0) {
    Span& span = _spans.at(_hole_span);
    span.top = AtomicAccess::load(&_hole_top);
    AtomicAccess::add(&_pages->stats()->hole_bytes_allocated, pointer_delta(span.top, span.start, 1));
    _hole_span = -1;
  }
  AtomicAccess::store(&_hole_top, _hole_end);   // empty window
}

// Moves to a hole of at least min_words words: the next one of the recycled
// page being worked on, or the first of the next page on the recyclable list.
// False if there is none. Holes that are too small are passed over and stay free.
// Called with _lock held.
bool EpoxyRegion::next_hole(size_t min_words) {
  close_hole();
  size_t min_lines = align_up(min_words * HeapWordSize, EpoxyLineSize) / EpoxyLineSize;
  while (true) {
    if (_recycled == nullptr) {
      EpoxyPage* page = _pages->take_recyclable(this, min_lines);
      if (page == nullptr) {
        return false;
      }
      link(page);
      _recycled = page;
      _cursor = page->bottom();
    }
    HeapWord* start;
    HeapWord* end;
    if (_pages->find_hole(_cursor, _recycled->end(), min_lines, &start, &end)) {
      _pages->take_hole(start, end);
      _cursor = end;
      _spans.push(Span{start, start, nullptr});
      _hole_span = _spans.length() - 1;
      _hole_end = end;
      AtomicAccess::release_store(&_hole_top, start);
      return true;
    }
    _recycled = nullptr;   // the page stays ours; its remaining holes are too small for this request
  }
}

HeapWord* EpoxyRegion::allocate(size_t size_in_words) {
  assert(is_object_aligned(size_in_words), "size must be object aligned: %zu", size_in_words);

  if (size_in_words > _page_words) {
    return allocate_run(size_in_words);
  }

  const bool holes = _scratch && size_in_words <= _max_recycle_words;
  while (true) {
    if (holes) {
      size_t actual;
      HeapWord* result = bump_hole(size_in_words, size_in_words, &actual);
      if (result != nullptr) {
        return result;
      }
    }

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

    // The current page cannot take this request: serve it from the next hole
    // of a recycled page, or from a new page.
    // Every page keeps its top(), so it stays walkable. This holds only while
    // pages are never taken away from a region: a thread that still holds a
    // pointer to the old page may bump it a little further.
    MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
    if (AtomicAccess::load(&_current) != page) {
      continue;  // another thread already moved on
    }
    if (holes && next_hole(size_in_words)) {
      continue;
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

  const bool holes = _scratch && min_words <= _max_recycle_words;
  while (true) {
    if (holes) {
      HeapWord* result = bump_hole(min_words, desired_words, actual_words);
      if (result != nullptr) {
        return result;
      }
    }

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
    // A TLAB in a tiny hole is refilled at once: ask for a hole worth having.
    // Every recyclable page has one of EpoxyMinRecycleLines lines.
    if (holes && next_hole(MAX2(min_words, MIN2(desired_words, (size_t)EpoxyMinRecycleLines * EpoxyLineSize / HeapWordSize)))) {
      continue;
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
  if (!_scratch) {
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
    return;
  }

  // A recycled page also holds promoted objects of earlier regions between
  // the holes. They are not walked here: the heap visits them by mark bit.
  for (int i = 0; i < _spans.length(); i++) {
    EpoxySpan span = span_at(i);
    HeapWord* p = span.start;
    while (p < span.top) {
      oop obj = cast_to_oop(p);
      size_t size = obj->size();
      cl->do_object(obj);
      p += size;
    }
    assert(p == span.top, "span must be walkable to exactly its top");
  }
}

void EpoxyRegion::release(EpoxyPage** kept) {
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);

  close_hole();

  EpoxyPage* kept_list = nullptr;
  size_t lines_freed = 0;
  size_t lines_kept = 0;
  size_t pages_freed = 0;
  size_t pages_recycled = 0;
  size_t pages_adopted = 0;
  EpoxyPage* page = _first;
  while (page != nullptr) {
    EpoxyPage* next = page->next();
    if (page->run_length() > 1) {
      // One large object. Promotion made every line it covers SCRATCH_PROMOTED.
      size_t run_pages = page->run_length();
      if (_pages->line_state(page->bottom()) == EpoxyLineScratchPromoted) {
        lines_kept += align_up(pointer_delta(page->top(), page->bottom(), 1), EpoxyLineSize) / EpoxyLineSize;
        pages_adopted += run_pages;
        _pages->set_scratch(page, false);
        page->set_owner(nullptr);
        page->set_next(kept_list);
        kept_list = page;
      } else {
        lines_freed += run_pages * _pages->lines_per_page();
        pages_freed += run_pages;
        _pages->free(page);
      }
    } else {
      EpoxyPageRelease result;
      _pages->release_page(page, &result);
      lines_freed += result.lines_freed;
      lines_kept += result.lines_kept;
      if (result.freed) {
        pages_freed++;
      } else {
        // The page has kept lines: its tail is no longer unused memory, any
        // free line may be handed out again.
        page->set_top(page->end());
        page->set_owner(nullptr);
        if (result.max_hole_lines >= (size_t)EpoxyMinRecycleLines) {
          _pages->add_recyclable(page, result.max_hole_lines);
          pages_recycled++;
        } else {
          page->set_next(kept_list);
          kept_list = page;
          pages_adopted++;
        }
      }
    }
    page = next;
  }

  AtomicAccess::release_store(&_current, (EpoxyPage*)nullptr);
  _first = nullptr;
  _last = nullptr;
  _recycled = nullptr;
  _cursor = nullptr;
  _hole_end = nullptr;
  AtomicAccess::store(&_hole_top, (HeapWord*)nullptr);
  _spans.clear();
  AtomicAccess::store(&_owned_pages, (size_t)0);
  *kept = kept_list;

  if (_scratch) {
    EpoxyLineStats* stats = _pages->stats();
    AtomicAccess::add(&stats->lines_freed, lines_freed);
    AtomicAccess::add(&stats->lines_kept, lines_kept);
    AtomicAccess::add(&stats->pages_freed, pages_freed);
    AtomicAccess::add(&stats->pages_recycled, pages_recycled);
    AtomicAccess::add(&stats->pages_adopted, pages_adopted);
    size_t released = AtomicAccess::add(&stats->regions_released, (size_t)1);
    log_debug(gc, region)("Epoxy region released: %zu lines freed, %zu lines kept, %zu pages freed, "
                          "%zu pages recycled, %zu pages adopted",
                          lines_freed, lines_kept, pages_freed, pages_recycled, pages_adopted);
    if (released % StatsLogInterval == 0) {
      _pages->log_stats(false);
    }
  }
}

size_t EpoxyRegion::used_bytes() const {
  return AtomicAccess::load(&_owned_pages) * _pages->page_size_bytes();
}
