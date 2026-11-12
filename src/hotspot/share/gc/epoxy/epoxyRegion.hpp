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

  EpoxyPage* volatile _current;      // page small allocations bump into; null before the first one
  EpoxyPage*          _first;        // pages in the order they were taken
  EpoxyPage*          _last;
  size_t volatile     _owned_pages;
  Mutex               _lock;         // taken to change page; never on the bump path

  void append(EpoxyPage* page);
  HeapWord* allocate_run(size_t size_in_words);

public:
  EpoxyRegion(EpoxyPageAllocator* pages, const char* name);

  // Thread-safe. Returns size_in_words of memory, or null when no pages are
  // left. size_in_words must be object aligned. A request larger than one
  // page gets a contiguous run of pages to itself.
  HeapWord* allocate(size_t size_in_words);

  // Every object in every page, in the order the pages were taken. Requires
  // that no thread is allocating (a safepoint, or a single-threaded caller).
  void object_iterate(ObjectClosure* cl);

  size_t used_bytes() const;
  const char* name() const { return _name; }
};

#endif // SHARE_GC_EPOXY_EPOXYREGION_HPP
