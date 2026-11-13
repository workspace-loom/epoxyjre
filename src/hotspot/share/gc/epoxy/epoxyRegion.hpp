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

#ifndef SHARE_GC_EPOXY_EPOXYREGION_HPP
#define SHARE_GC_EPOXY_EPOXYREGION_HPP

#include "gc/epoxy/epoxyPage.hpp"
#include "memory/allocation.hpp"
#include "runtime/mutex.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"

class EpoxyPageAllocator;
class ObjectClosure;

// One stretch of memory a scratch region allocated in: [start, top) is
// walkable from start to top.
struct EpoxySpan {
  HeapWord* start;
  HeapWord* top;
};

// A set of pages whose objects share one lifetime, with bump-pointer
// allocation.
//
// A region takes fresh pages from the page allocator. A scratch region also
// allocates in the free lines of kept pages (see EpoxyPageAllocator): a hole,
// a run of free lines inside a page that holds promoted objects of earlier
// regions. Such a page is not walkable from bottom() to top(); what a region
// allocated in is recorded as spans, and every span is walkable.
//
// Space between a fresh page's top() and end() is unused: the region leaves it
// behind when a request does not fit and it moves to a new page.
//
// The global region is allocated in by many threads (lock-free bump of
// EpoxyPage::_top); it never uses recycled pages and keeps no spans. A scratch
// region is allocated in by the thread that owns it only.
class EpoxyRegion : public CHeapObj<mtGC> {
private:
  struct Span {
    HeapWord* start;
    HeapWord* top;     // final top; meaningless while page != null or while the span is the current hole
    EpoxyPage* page;   // fresh page or run: its top() is the span's top
  };

  EpoxyPageAllocator* const _pages;
  const char* const         _name;
  const size_t              _page_words;
  const size_t              _max_recycle_words;  // requests larger than this skip recycled pages
  const bool                _scratch;
  EpoxyRegion*              _parent;       // enclosing scratch region of the same thread
  uint                      _depth;        // 0 for a region with no parent
  intptr_t*                 _scope_frame;  // frame pointer of the method activation that opened it, or null

  EpoxyPage* volatile _current;      // fresh page small allocations bump into; null before the first one
  EpoxyPage*          _first;        // pages (fresh and recycled) in the order they were taken
  EpoxyPage*          _last;
  size_t volatile     _owned_pages;
  Mutex               _lock;         // taken to change page or hole; never on the bump path

  // Scratch regions only. The hole window [_hole_top, _hole_end) is what is
  // left of the hole being bumped in; it is empty when _hole_top == _hole_end.
  HeapWord* volatile  _hole_top;
  HeapWord*           _hole_end;
  int                 _hole_span;    // index in _spans of the current hole, or -1
  EpoxyPage*          _recycled;     // recycled page whose holes are being handed out, or null
  HeapWord*           _cursor;       // holes of _recycled before this address have been looked at
  GrowableArrayCHeap<Span, mtGC> _spans;

  void link(EpoxyPage* page);
  void append(EpoxyPage* page);
  HeapWord* allocate_run(size_t size_in_words);

  HeapWord* bump_hole(size_t min_words, size_t desired_words, size_t* actual_words);
  bool next_hole(size_t min_words);
  void close_hole();

public:
  EpoxyRegion(EpoxyPageAllocator* pages, const char* name, bool scratch = false);

  bool is_scratch() const          { return _scratch; }
  EpoxyRegion* parent() const      { return _parent; }
  uint depth() const               { return _depth; }
  intptr_t* scope_frame() const    { return _scope_frame; }
  void set_scope_frame(intptr_t* fp) { _scope_frame = fp; }
  void set_parent(EpoxyRegion* parent) {
    _parent = parent;
    _depth = (parent == nullptr) ? 0 : parent->depth() + 1;
  }

  // Thread-safe for the global region; for a scratch region only its own
  // thread may call. Returns size_in_words of memory, or null when no pages
  // are left. size_in_words must be object aligned. A request larger than one
  // page gets a contiguous run of pages to itself. A scratch region serves a
  // request of at most EpoxyMaxRecycleBytes from the holes of recycled pages
  // first.
  HeapWord* allocate(size_t size_in_words);

  // For a caller that can use any size between min_words and desired_words (a
  // TLAB refill): takes what is left of the current hole or page when that is
  // at least min_words, so the tail is not abandoned. What it returns lies
  // entirely in one hole or one page. desired_words must not exceed one page.
  // Sets *actual_words; returns null when no pages are left.
  HeapWord* allocate(size_t min_words, size_t desired_words, size_t* actual_words);

  // Every object this region allocated, span by span in the order the spans
  // were taken (a global region: page by page). Objects of earlier regions
  // that sit in the kept lines of a recycled page are not visited. Requires
  // that no thread is allocating (a safepoint, or a single-threaded caller).
  void object_iterate(ObjectClosure* cl);

  // Gives up every page. For each page, lines holding a promoted object become
  // kept and the rest of what the region allocated in becomes free; a page
  // with nothing kept is freed without being looked at, a page with a hole of
  // at least EpoxyMinRecycleLines lines goes to the page allocator's
  // recyclable list, and every other page is handed back through *kept as a
  // list linked by next(), owned by nobody. A run of pages (one large object)
  // is kept whole if its object is promoted, else freed. Whether an object is
  // promoted is read from the line table only. Only for a region that no
  // thread is allocating in. The region is empty afterwards and may be used again.
  void release(EpoxyPage** kept);

  // First page; follow EpoxyPage::next(). Only for a region nobody is allocating in.
  EpoxyPage* first_page() const { return _first; }

  // The spans allocated in so far, for a region nobody is allocating in.
  // A global region has none.
  int span_count() const { return _spans.length(); }
  EpoxySpan span_at(int index) const;

  size_t used_bytes() const;
  const char* name() const { return _name; }
};

#endif // SHARE_GC_EPOXY_EPOXYREGION_HPP
