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
#include "runtime/atomicAccess.hpp"
#include "runtime/mutex.hpp"
#include "utilities/bitMap.hpp"

class MarkBitMap;

// What EpoxyPageAllocator::release_page found in, and did to, the lines of one page.
struct EpoxyPageRelease {
  bool   freed;           // no kept line: the whole page was freed
  size_t lines_freed;     // lines that became free (scratch, nothing promoted)
  size_t lines_kept;      // lines that became kept (scratch, holding a promoted object)
  size_t max_hole_lines;  // longest run of free lines in the page afterwards
};

// Running totals, for the statistics logged at exit. Updated with atomic adds.
struct EpoxyLineStats {
  volatile size_t regions_released;
  volatile size_t lines_freed;
  volatile size_t lines_kept;
  volatile size_t pages_freed;
  volatile size_t pages_recycled;      // put on the recyclable list
  volatile size_t pages_adopted;       // kept without recycling
  volatile size_t pages_reused;        // taken from the recyclable list by a region
  volatile size_t hole_bytes_allocated;  // bytes bump-allocated in recycled holes
};

// Hands out and takes back fixed-size pages of a reserved address range.
//
// Memory is committed the first time a page is handed out and stays
// committed afterwards. Freeing never visits the contents of a page.
// allocate() and free() may be called from any thread.
//
// Alongside the pages it keeps the line table: one byte per 256-byte line of
// the whole heap (see EpoxyLineState). is_scratch() reads it, from any thread
// at any time, so every access to a table byte is an atomic byte load or
// store. A line is written only by the thread whose region owns its page,
// except lines of pages on no region's chain, which nobody allocates in.
//
// It also keeps the recyclable list: pages that hold kept lines and a hole of
// free lines big enough to allocate in again.
class EpoxyPageAllocator : public CHeapObj<mtGC> {
private:
  const MemRegion _reserved;
  const size_t    _page_size_bytes;
  const int       _page_shift;
  const size_t    _max_pages;
  const size_t    _lines_per_page;

  EpoxyPage*      _pages;       // one entry per page, indexed by page number
  uint8_t*        _lines;       // one byte per line of the heap
  size_t          _lines_bytes; // reserved size of _lines
  MarkBitMap*     _marks;       // promotion marks, cleared for memory that is freed; may be null
  CHeapBitMap     _free;        // bit set: the page is free
  CHeapBitMap     _committed;   // bit set: the page's memory is committed
  size_t          _used_pages;
  size_t          _committed_bytes;
  EpoxyPage*      _recyclable;  // list linked by EpoxyPage::next(); guarded by _lock
  size_t          _recyclable_count;
  EpoxyLineStats  _stats;
  mutable Mutex   _lock;

  char* page_base(size_t index) const {
    return (char*)_reserved.start() + (index << _page_shift);
  }

  size_t line_index(const void* addr) const {
    return pointer_delta(addr, _reserved.start(), 1) >> EpoxyLineShift;
  }
  HeapWord* line_address(size_t index) const {
    return (HeapWord*)((char*)_reserved.start() + (index << EpoxyLineShift));
  }

  // First index of a run of num_pages free pages, or _max_pages if none.
  size_t find_free_run(size_t num_pages) const;
  bool commit_run(size_t start, size_t num_pages);
  void set_lines(size_t first, size_t last, uint8_t state);

public:
  // reserved must be aligned to page_size_bytes at both ends.
  // pre_committed: the whole range is already committed (explicit large pages).
  EpoxyPageAllocator(MemRegion reserved, size_t page_size_bytes, bool pre_committed = false);
  ~EpoxyPageAllocator();

  // A contiguous run with top() == bottom(), or null when no run fits. All its
  // lines are kept (0): the global region's state.
  EpoxyPage* allocate(uint num_pages);

  // Returns the whole run that first starts.
  void free(EpoxyPage* first);

  // First page of the allocated run containing addr; null for an address
  // outside the heap or in a free page.
  EpoxyPage* page_for(const void* addr) const;

  // ---- line table ----

  uint8_t line_state(const void* addr) const {
    return AtomicAccess::load(&_lines[line_index(addr)]);
  }
  // Scratch objects are the ones that may be freed with their region: bit 0
  // of the line byte. The answer comes from a byte table so that compiled
  // code can ask with one load.
  bool is_scratch(const void* addr) const {
    return _reserved.contains(addr) && (line_state(addr) & 1) != 0;
  }
  // Sets every line of the run starting at first to scratch (a region takes a
  // fresh run) or to kept (a run that survives its region).
  void set_scratch(EpoxyPage* first, bool scratch);
  // Every line that [start, end) touches becomes SCRATCH_PROMOTED. The range
  // must lie in lines that belong to a live scratch region.
  void mark_lines_promoted(const void* start, const void* end);

  // Gives up one page of a scratch region: lines that are 3 become 0, lines
  // that are 1 become 2 (poisoned in debug builds, promotion marks cleared).
  // A page with no kept line at all is freed whole instead (out->freed). The
  // caller sets owner, top and list membership of a page that is kept.
  void release_page(EpoxyPage* page, EpoxyPageRelease* out);

  // The first hole of at least min_lines lines (a maximal run of free lines)
  // that starts at or after from and ends within [from, limit); from and
  // limit are line aligned. False if there is none.
  bool find_hole(HeapWord* from, HeapWord* limit, size_t min_lines, HeapWord** start, HeapWord** end) const;
  // The region takes the hole: its free lines become scratch.
  void take_hole(HeapWord* start, HeapWord* end);

  // ---- recyclable list ----

  void add_recyclable(EpoxyPage* page);
  // Removes a page from the list and makes owner its owner; null if the list is empty.
  EpoxyPage* take_recyclable(EpoxyRegion* owner);
  // For walks at a safepoint, which need no lock.
  EpoxyPage* recyclable_pages() const { return _recyclable; }
  size_t recyclable_count() const;

  EpoxyLineStats* stats() { return &_stats; }
  // Logs the totals at info level (gc+region). with_small_holes adds the
  // number of free lines in holes smaller than MinTLABSize, which takes a scan
  // of the line table.
  void log_stats(bool with_small_holes) const;
  // Free lines that sit in holes of fewer than min_lines lines. A scan of the table.
  size_t free_lines_in_holes_below(size_t min_lines) const;

  void set_marks(MarkBitMap* marks) { _marks = marks; }
  const uint8_t* line_table() const { return _lines; }
  int page_shift() const           { return _page_shift; }

  size_t page_size_bytes() const { return _page_size_bytes; }
  size_t max_pages() const       { return _max_pages; }
  size_t lines_per_page() const  { return _lines_per_page; }
  size_t used_pages() const;
  size_t committed_bytes() const;
  MemRegion reserved() const     { return _reserved; }
};

#endif // SHARE_GC_EPOXY_EPOXYPAGEALLOCATOR_HPP
