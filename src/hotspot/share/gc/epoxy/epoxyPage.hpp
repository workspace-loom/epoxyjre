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

#ifndef SHARE_GC_EPOXY_EPOXYPAGE_HPP
#define SHARE_GC_EPOXY_EPOXYPAGE_HPP

#include "utilities/globalDefinitions.hpp"

class EpoxyPageAllocator;
class EpoxyRegion;

// Metadata for one heap page. Instances live in a side array owned by
// EpoxyPageAllocator, never inside the page itself, so freeing a page never
// has to touch its memory.
//
// Pages are handed out in runs of one or more contiguous pages. Only the
// first page of a run is given to callers: its end() covers the whole run.
class EpoxyPage {
  friend class EpoxyPageAllocator;

private:
  HeapWord*          _bottom;
  HeapWord*          _end;
  HeapWord* volatile _top;
  uint               _run_length;  // pages in the run this page starts; 0 inside a run or when free
  uint               _run_start;   // index of the first page of the run this page belongs to
  EpoxyRegion*       _owner;
  EpoxyPage*         _next;

public:
  HeapWord* bottom() const            { return _bottom; }
  HeapWord* end() const               { return _end; }
  HeapWord* top() const               { return _top; }
  void set_top(HeapWord* top)         { _top = top; }
  HeapWord* volatile* top_addr()      { return &_top; }

  uint run_length() const             { return _run_length; }

  EpoxyRegion* owner() const          { return _owner; }
  void set_owner(EpoxyRegion* owner)  { _owner = owner; }

  EpoxyPage* next() const             { return _next; }
  void set_next(EpoxyPage* next)      { _next = next; }
};

#endif // SHARE_GC_EPOXY_EPOXYPAGE_HPP
