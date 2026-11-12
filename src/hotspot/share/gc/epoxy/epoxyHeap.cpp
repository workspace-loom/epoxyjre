/*
 * Copyright (c) 2023, 2026, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2017, 2022, Red Hat, Inc. All rights reserved.
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

#include "gc/epoxy/epoxyHeap.hpp"
#include "gc/epoxy/epoxyInitLogger.hpp"
#include "gc/epoxy/epoxyMemoryPool.hpp"
#include "gc/epoxy/epoxyPageAllocator.hpp"
#include "gc/epoxy/epoxyRegion.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/locationPrinter.inline.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "runtime/globals.hpp"
#include "services/memoryUsage.hpp"
#include "utilities/ostream.hpp"

jint EpoxyHeap::initialize() {
  _page_size_bytes = EpoxyPageSize;

  // The heap is a whole number of pages: round the maximum size down.
  size_t max_byte_size = align_down(MaxHeapSize, _page_size_bytes);
  if (max_byte_size / _page_size_bytes < MinPages) {
    vm_exit_during_initialization("Epoxy heap is too small: at least 16 pages of EpoxyPageSize are required");
  }

  // Reserve the address range. Pages are committed as they are first handed out.
  ReservedHeapSpace heap_rs = Universe::reserve_heap(max_byte_size, HeapAlignment);
  initialize_reserved_region(heap_rs);
  assert(is_aligned(_reserved.start(), _page_size_bytes), "heap start must be page aligned");

  MemRegion pages_region(_reserved.start(), max_byte_size / HeapWordSize);
  _page_allocator = new EpoxyPageAllocator(pages_region, _page_size_bytes, heap_rs.special());
  _global_region = new EpoxyRegion(_page_allocator, "global");

  // Precompute hot fields. A TLAB never spans pages.
  _max_tlab_size = MIN2(CollectedHeap::max_tlab_size(), align_object_size(_page_size_bytes / HeapWordSize));
  _step_counter_update = MIN2<size_t>(max_byte_size / 16, EpoxyUpdateCountersStep);
  _step_heap_print = (EpoxyPrintHeapSteps == 0) ? SIZE_MAX : (max_byte_size / EpoxyPrintHeapSteps);

  // Enable monitoring
  _monitoring_support = new EpoxyMonitoringSupport(this);

  // Install barrier set
  BarrierSet::set_barrier_set(new EpoxyBarrierSet());

  // All done, print out the configuration
  EpoxyInitLogger::print();

  return JNI_OK;
}

size_t EpoxyHeap::capacity() const {
  return _page_allocator->committed_bytes();
}

size_t EpoxyHeap::used() const {
  return _page_allocator->used_pages() * _page_size_bytes;
}

size_t EpoxyHeap::unused() const {
  size_t used = this->used();
  size_t committed = capacity();
  return committed - used;
}

MemoryUsage EpoxyHeap::memory_usage() {
  size_t used = this->used();
  size_t committed = capacity();
  return MemoryUsage(InitialHeapSize, used, committed, max_capacity());
}

bool EpoxyHeap::is_in(const void* p) const {
  EpoxyPage* page = _page_allocator->page_for(p);
  return page != nullptr && p < (const void*)page->top();
}

void EpoxyHeap::initialize_serviceability() {
  _pool = new EpoxyMemoryPool(this);
  _memory_manager.add_pool(_pool);
}

void EpoxyHeap::post_initialize() {
  CollectedHeap::post_initialize();
  _monitoring_support->mark_ready();
}

GrowableArray<GCMemoryManager*> EpoxyHeap::memory_managers() {
  GrowableArray<GCMemoryManager*> memory_managers(1);
  memory_managers.append(&_memory_manager);
  return memory_managers;
}

GrowableArray<MemoryPool*> EpoxyHeap::memory_pools() {
  GrowableArray<MemoryPool*> memory_pools(1);
  memory_pools.append(_pool);
  return memory_pools;
}

size_t EpoxyHeap::unsafe_max_tlab_alloc() const {
  // Return max allocatable TLAB size, and let allocation path figure out
  // the actual allocation size. Note: result should be in bytes.
  return _max_tlab_size * HeapWordSize;
}

EpoxyHeap* EpoxyHeap::heap() {
  return named_heap<EpoxyHeap>(CollectedHeap::Epoxy);
}

HeapWord* EpoxyHeap::allocate_work(size_t size) {
  assert(is_object_aligned(size), "Allocation size should be aligned: %zu", size);

  HeapWord* res = _global_region->allocate(size);
  if (res == nullptr) {
    return nullptr;
  }

  size_t used = this->used();

  // Allocation successful, update counters and print status.
  // At this point, some diagnostic subsystems might not yet be initialized.
  // We pretend the printout happened either way. This keeps allocation path
  // from obsessively checking the subsystems' status on every allocation.
  size_t last_counter = _last_counter_update.load_relaxed();
  if ((used - last_counter >= _step_counter_update) &&
      _last_counter_update.compare_set(last_counter, used)) {
    if (_monitoring_support->is_ready()) {
      _monitoring_support->update_counters();
    }
  }

  size_t last_heap = _last_heap_print.load_relaxed();
  if ((used - last_heap >= _step_heap_print) &&
      _last_heap_print.compare_set(last_heap, used)) {
    print_heap_info(used);
    if (Metaspace::initialized()) {
      print_metaspace_info();
    }
  }

  assert(is_object_aligned(res), "Object should be aligned: " PTR_FORMAT, p2i(res));
  return res;
}

HeapWord* EpoxyHeap::allocate_new_tlab(size_t min_size,
                                       size_t requested_size,
                                       size_t* actual_size) {
  // Always honor boundaries and alignment. A TLAB is at most one page.
  size_t size = clamp(requested_size, min_size, _max_tlab_size);
  size = align_up(size, MinObjAlignment);

  assert(min_size <= size, "Size honors min size: %zu <= %zu", min_size, size);
  assert(size <= _max_tlab_size, "Size honors max size: %zu <= %zu", size, _max_tlab_size);

  if (log_is_enabled(Trace, gc)) {
    ResourceMark rm;
    log_trace(gc)("TLAB size for \"%s\" (Requested: %zuK, Min: %zuK, Max: %zuK) -> %zuK",
                  Thread::current()->name(),
                  requested_size * HeapWordSize / K,
                  min_size * HeapWordSize / K,
                  _max_tlab_size * HeapWordSize / K,
                  size * HeapWordSize / K);
  }

  // Take the rest of the current page if it can hold at least min_size.
  return _global_region->allocate(align_up(min_size, MinObjAlignment), size, actual_size);
}

HeapWord* EpoxyHeap::mem_allocate(size_t size) {
  return allocate_work(size);
}

HeapWord* EpoxyHeap::allocate_loaded_archive_space(size_t size) {
  return allocate_work(size);
}

void EpoxyHeap::collect(GCCause::Cause cause) {
  switch (cause) {
    case GCCause::_metadata_GC_threshold:
    case GCCause::_metadata_GC_clear_soft_refs:
      // Receiving these causes means the VM itself entered the safepoint for metadata collection.
      // While Epoxy does not do GC, it has to perform sizing adjustments, otherwise we would
      // re-enter the safepoint again very soon.

      assert(SafepointSynchronize::is_at_safepoint(), "Expected at safepoint");
      log_info(gc)("GC request for \"%s\" is handled", GCCause::to_string(cause));
      MetaspaceGC::compute_new_size();
      print_metaspace_info();
      break;
    default:
      log_info(gc)("GC request for \"%s\" is ignored", GCCause::to_string(cause));
  }
  _monitoring_support->update_counters();
}

void EpoxyHeap::do_full_collection(bool clear_all_soft_refs) {
  collect(gc_cause());
}

void EpoxyHeap::object_iterate(ObjectClosure *cl) {
  _global_region->object_iterate(cl);
}

void EpoxyHeap::print_heap_on(outputStream *st) const {
  st->print_cr("Epoxy Heap");

  StreamIndentor si(st, 1);

  st->print_cr("Reserved: [" PTR_FORMAT ", " PTR_FORMAT "), %zuK",
               p2i(_reserved.start()), p2i(_reserved.end()), _reserved.byte_size() / K);
  if (_page_allocator != nullptr) {
    st->print_cr("Pages: %zuK each, %zu of %zu in use, %zuK committed",
                 _page_size_bytes / K,
                 _page_allocator->used_pages(), _page_allocator->max_pages(),
                 _page_allocator->committed_bytes() / K);
  }
}

bool EpoxyHeap::print_location(outputStream* st, void* addr) const {
  return BlockLocationPrinter<EpoxyHeap>::print_location(st, addr);
}

void EpoxyHeap::print_tracing_info() const {
  print_heap_info(used());
  print_metaspace_info();
}

void EpoxyHeap::print_heap_info(size_t used) const {
  size_t reserved  = max_capacity();
  size_t committed = capacity();

  if (reserved != 0) {
    log_info(gc)("Heap: %zu%s reserved, %zu%s (%.2f%%) committed, "
                 "%zu%s (%.2f%%) used",
            byte_size_in_proper_unit(reserved),  proper_unit_for_byte_size(reserved),
            byte_size_in_proper_unit(committed), proper_unit_for_byte_size(committed),
            committed * 100.0 / reserved,
            byte_size_in_proper_unit(used),      proper_unit_for_byte_size(used),
            used * 100.0 / reserved);
  } else {
    log_info(gc)("Heap: no reliable data");
  }
}

void EpoxyHeap::print_metaspace_info() const {
  MetaspaceCombinedStats stats = MetaspaceUtils::get_combined_statistics();
  size_t reserved  = stats.reserved();
  size_t committed = stats.committed();
  size_t used      = stats.used();

  if (reserved != 0) {
    log_info(gc, metaspace)("Metaspace: %zu%s reserved, %zu%s (%.2f%%) committed, "
                            "%zu%s (%.2f%%) used",
            byte_size_in_proper_unit(reserved),  proper_unit_for_byte_size(reserved),
            byte_size_in_proper_unit(committed), proper_unit_for_byte_size(committed),
            committed * 100.0 / reserved,
            byte_size_in_proper_unit(used),      proper_unit_for_byte_size(used),
            used * 100.0 / reserved);
  } else {
    log_info(gc, metaspace)("Metaspace: no reliable data");
  }
}
