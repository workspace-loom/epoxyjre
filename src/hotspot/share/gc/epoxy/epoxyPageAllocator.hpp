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

#ifndef SHARE_GC_EPOXY_EPOXYPAGEALLOCATOR_HPP
#define SHARE_GC_EPOXY_EPOXYPAGEALLOCATOR_HPP

#include "gc/epoxy/epoxyPage.hpp"
#include "memory/allocation.hpp"
#include "memory/memRegion.hpp"
#include "runtime/mutex.hpp"
#include "utilities/bitMap.hpp"

// Hands out and takes back fixed-size pages of a reserved address range.
//
// Memory is committed the first time a page is handed out and stays
// committed afterwards. Freeing never visits the contents of a page.
// allocate() and free() may be called from any thread.
class EpoxyPageAllocator : public CHeapObj<mtGC> {
private:
  const MemRegion _reserved;
  const size_t    _page_size_bytes;
  const int       _page_shift;
  const size_t    _max_pages;

  EpoxyPage*      _pages;       // one entry per page, indexed by page number
  CHeapBitMap     _free;        // bit set: the page is free
  CHeapBitMap     _committed;   // bit set: the page's memory is committed
  size_t          _used_pages;
  size_t          _committed_bytes;
  mutable Mutex   _lock;

  char* page_base(size_t index) const {
    return (char*)_reserved.start() + (index << _page_shift);
  }

  // First index of a run of num_pages free pages, or _max_pages if none.
  size_t find_free_run(size_t num_pages) const;
  bool commit_run(size_t start, size_t num_pages);

public:
  // reserved must be aligned to page_size_bytes at both ends.
  EpoxyPageAllocator(MemRegion reserved, size_t page_size_bytes);
  ~EpoxyPageAllocator();

  // A contiguous run with top() == bottom(), or null when no run fits.
  EpoxyPage* allocate(uint num_pages);

  // Returns the whole run that first starts.
  void free(EpoxyPage* first);

  // First page of the allocated run containing addr; null for an address
  // outside the heap or in a free page.
  EpoxyPage* page_for(const void* addr) const;

  size_t page_size_bytes() const { return _page_size_bytes; }
  size_t max_pages() const       { return _max_pages; }
  size_t used_pages() const;
  size_t committed_bytes() const;
  MemRegion reserved() const     { return _reserved; }
};

#endif // SHARE_GC_EPOXY_EPOXYPAGEALLOCATOR_HPP
