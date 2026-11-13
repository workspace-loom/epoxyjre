/*
 * Copyright (c) 2023, 2025, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2017, 2018, Red Hat, Inc. All rights reserved.
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
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "gc/epoxy/epoxyBarrierSet.inline.hpp"
#include "gc/epoxy/epoxyBarrierSetAssembler.hpp"
#include "gc/epoxy/epoxyPage.hpp"
#include "gc/epoxy/epoxyRegion.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "gc/epoxy/epoxyHeap.hpp"
#include "gc/epoxy/epoxyThreadLocalData.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "runtime/javaThread.hpp"
#ifdef COMPILER1
#include "gc/epoxy/c1/epoxyBarrierSetC1.hpp"
#include "gc/shared/c1/barrierSetC1.hpp"
#endif
#ifdef COMPILER2
#include "gc/epoxy/c2/epoxyBarrierSetC2.hpp"
#include "gc/shared/c2/barrierSetC2.hpp"
#endif

EpoxyBarrierSet::EpoxyBarrierSet() : BarrierSet(
          make_barrier_set_assembler<EpoxyBarrierSetAssembler>(),
          make_barrier_set_c1<EpoxyBarrierSetC1>(),
          make_barrier_set_c2<EpoxyBarrierSetC2>(),
          nullptr /* barrier_set_nmethod */,
          nullptr /* barrier_set_stack_chunk */,
          BarrierSet::FakeRtti(BarrierSet::EpoxyBarrierSet)) {}

void EpoxyBarrierSet::on_thread_create(Thread *thread) {
  EpoxyThreadLocalData::create(thread);
}

void EpoxyBarrierSet::on_thread_destroy(Thread *thread) {
  assert(EpoxyThreadLocalData::scratch(thread) == nullptr, "regions are given up when the thread detaches");
  EpoxyThreadLocalData::destroy(thread);
}

void EpoxyBarrierSet::on_thread_detach(Thread *thread) {
  // Called while the thread is still on the threads list and holds
  // Threads_lock, so this cannot overlap a safepoint operation that walks
  // the heap. The thread's TLAB has been retired by now.
  EpoxyHeap::heap()->exit_all_scratch(thread);
}

// True if inner is outer or a region nested inside it on the same thread.
static bool is_nested_in(EpoxyRegion* inner, EpoxyRegion* outer) {
  for (EpoxyRegion* r = inner; r != nullptr; r = r->parent()) {
    if (r == outer) {
      return true;
    }
  }
  return false;
}

void EpoxyBarrierSet::on_store_slow(oop value, const void* addr, oop holder) {
  // value is an unpromoted scratch object: it belongs to a region of the
  // calling thread, and only this thread can see it.
  EpoxyHeap* heap = EpoxyHeap::heap();
  EpoxyPageAllocator* pages = heap->page_allocator();

  if (!pages->is_scratch(addr)) {
    // A native root, or an object that is not in any scratch region.
    heap->promote(value);
    return;
  }

  // The holder is in a scratch line. If it has been promoted it outlives
  // every region, wherever it lies, and other threads may reach it. When the
  // holder's start is unknown, assume so if the line holds part of a promoted
  // object: promotion marks every line the object covers.
  EpoxyPage* holder_page = pages->page_for(addr);
  bool holder_promoted = (holder != nullptr) ? heap->is_promoted(holder)
                                             : pages->line_state(addr) == EpoxyLineScratchPromoted;
  if (holder_promoted) {
    heap->promote(value);
    return;
  }

  // The holder is an unpromoted scratch object too, so it is private to this
  // thread and both regions are this thread's: their descriptors can be read.
  // The store is fine if the holder dies no later than the value: same
  // region, or the holder's region is nested inside the value's.
  EpoxyRegion* value_region = pages->page_for(cast_from_oop<void*>(value))->owner();
  EpoxyRegion* holder_region = holder_page->owner();
  if (!is_nested_in(holder_region, value_region)) {
    heap->promote(value);
  }
}

// Before count references are copied from src to dst (addresses of the first
// elements). The destination array's start is not known here, so the holder
// is judged by its page.
JRT_LEAF(void, EpoxyBarrierSet::arraycopy_barrier_entry(void* src, void* dst, size_t count))
  if (UseCompressedOops) {
    narrowOop* p = (narrowOop*)src;
    for (size_t i = 0; i < count; i++) {
      narrowOop element = RawAccess<>::oop_load(p + i);
      if (!CompressedOops::is_null(element)) {
        EpoxyBarrierSet::on_store(CompressedOops::decode_not_null(element), dst, nullptr);
      }
    }
  } else {
    oop* p = (oop*)src;
    for (size_t i = 0; i < count; i++) {
      oop element = RawAccess<>::oop_load(p + i);
      if (element != nullptr) {
        EpoxyBarrierSet::on_store(element, dst, nullptr);
      }
    }
  }
JRT_END

JRT_LEAF(void, EpoxyBarrierSet::store_barrier_entry(oopDesc* value, oopDesc* holder))
  EpoxyBarrierSet::on_store(cast_to_oop(value), holder, cast_to_oop(holder));
JRT_END
