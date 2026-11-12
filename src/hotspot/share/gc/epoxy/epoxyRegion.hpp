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

class EpoxyPageAllocator;
class ObjectClosure;

// A set of pages whose objects share one lifetime, with bump-pointer
// allocation. Every page it owns is walkable from bottom() to top(). Space
// between a page's top() and end() is unused: the region leaves it behind
// when a request does not fit and it moves to a new page.
class EpoxyRegion : public CHeapObj<mtGC> {
private:
  EpoxyPageAllocator* const _pages;
  const char* const         _name;
  const size_t              _page_words;
  const bool                _scratch;
  EpoxyRegion*              _parent;       // enclosing scratch region of the same thread
  uint                      _depth;        // 0 for a region with no parent

  EpoxyPage* volatile _current;      // page small allocations bump into; null before the first one
  EpoxyPage*          _first;        // pages in the order they were taken
  EpoxyPage*          _last;
  size_t volatile     _owned_pages;
  Mutex               _lock;         // taken to change page; never on the bump path

  void append(EpoxyPage* page);
  HeapWord* allocate_run(size_t size_in_words);

public:
  EpoxyRegion(EpoxyPageAllocator* pages, const char* name, bool scratch = false);

  bool is_scratch() const          { return _scratch; }
  EpoxyRegion* parent() const      { return _parent; }
  uint depth() const               { return _depth; }
  void set_parent(EpoxyRegion* parent) {
    _parent = parent;
    _depth = (parent == nullptr) ? 0 : parent->depth() + 1;
  }

  // Thread-safe. Returns size_in_words of memory, or null when no pages are
  // left. size_in_words must be object aligned. A request larger than one
  // page gets a contiguous run of pages to itself.
  HeapWord* allocate(size_t size_in_words);

  // Thread-safe. For a caller that can use any size between min_words and
  // desired_words (a TLAB refill): takes what is left of the current page when
  // that is at least min_words, so the tail is not abandoned. desired_words
  // must not exceed one page. Sets *actual_words; returns null when no pages
  // are left.
  HeapWord* allocate(size_t min_words, size_t desired_words, size_t* actual_words);

  // Every object in every page, in the order the pages were taken. Requires
  // that no thread is allocating (a safepoint, or a single-threaded caller).
  void object_iterate(ObjectClosure* cl);

  // Gives up every page. A page with no promoted object is freed without
  // being looked at; the others are handed back through *kept as a list
  // linked by next(), no longer scratch and owned by nobody. Only for a
  // region that no thread is allocating in. The region is empty afterwards
  // and may be used again.
  void release(EpoxyPage** kept);

  size_t used_bytes() const;
  const char* name() const { return _name; }
};

#endif // SHARE_GC_EPOXY_EPOXYREGION_HPP
