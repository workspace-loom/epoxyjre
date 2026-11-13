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
#include "gc/shared/markBitMap.inline.hpp"
#include "utilities/copy.hpp"
#include "utilities/growableArray.hpp"
#include "gc/epoxy/epoxyMemoryPool.hpp"
#include "gc/epoxy/epoxyPageAllocator.hpp"
#include "gc/epoxy/epoxyProfile.hpp"
#include "gc/epoxy/epoxyRegion.hpp"
#include "gc/epoxy/epoxyScopes.hpp"
#include "gc/epoxy/epoxyThreadLocalData.hpp"
#include "gc/epoxy/epoxyVerifier.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/locationPrinter.inline.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/resourceArea.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/universe.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/klass.hpp"
#include "oops/oop.inline.hpp"
#include "oops/symbol.hpp"
#include "compiler/compilerDefinitions.inline.hpp"
#include "runtime/arguments.hpp"
#include "runtime/globals.hpp"
#include "runtime/java.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/threads.hpp"
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

  // Promotion marks: one bit per possible object start. The storage is
  // reserved in full and backed by the OS only where bits are set.
  size_t marks_bytes = align_up(MarkBitMap::compute_size(max_byte_size), os::vm_allocation_granularity());
  char* marks_base = os::reserve_memory(marks_bytes, mtGC);
  if (marks_base == nullptr) {
    vm_exit_during_initialization("Could not reserve memory for the Epoxy promotion marks");
  }
  os::commit_memory_or_exit(marks_base, marks_bytes, false /* executable */, "Epoxy promotion marks");
  _promoted_marks.initialize(pages_region, MemRegion((HeapWord*)marks_base, marks_bytes / HeapWordSize));
  _page_allocator->set_marks(&_promoted_marks);

  // Precompute hot fields. A TLAB never spans pages.
  _max_tlab_size = MIN2(CollectedHeap::max_tlab_size(), align_object_size(_page_size_bytes / HeapWordSize));
  _step_counter_update = MIN2<size_t>(max_byte_size / 16, EpoxyUpdateCountersStep);
  _step_heap_print = (EpoxyPrintHeapSteps == 0) ? SIZE_MAX : (max_byte_size / EpoxyPrintHeapSteps);

  // Methods that run in their own regions
  EpoxyScopes::initialize();

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

bool EpoxyHeap::is_promoted(oop obj) const {
  return _promoted_marks.is_marked(obj);
}

bool EpoxyHeap::mark_promoted(oop obj) {
  if (!_promoted_marks.par_mark(obj)) {
    return false;
  }
  // Every line the object covers is kept when its region goes. Only the thread
  // that owns the region promotes its objects, so nobody else writes these lines.
  HeapWord* start = cast_from_oop<HeapWord*>(obj);
  _page_allocator->mark_lines_promoted(start, start + obj->size());
  _page_allocator->page_for(start)->note_promoted();
  if (EpoxyProfile) {
    EpoxyProfiler::note_promoted(obj->klass(), obj->size());
  }
  return true;
}

// Marks every unpromoted scratch object referred to by the object it is applied to.
class EpoxyPromoteClosure : public BasicOopIterateClosure {
  EpoxyHeap* const _heap;
  EpoxyPageAllocator* const _pages;
  GrowableArrayCHeap<oop, mtGC>* const _stack;

  template <typename T>
  void do_oop_work(T* p) {
    T element = RawAccess<>::oop_load(p);
    if (CompressedOops::is_null(element)) {
      return;
    }
    oop obj = CompressedOops::decode_not_null(element);
    if (_pages->is_scratch(cast_from_oop<void*>(obj)) && _heap->mark_promoted(obj)) {
      _stack->push(obj);
    }
  }

public:
  EpoxyPromoteClosure(EpoxyHeap* heap, GrowableArrayCHeap<oop, mtGC>* stack) :
      _heap(heap), _pages(heap->page_allocator()), _stack(stack) {}

  void do_oop(oop* p) override       { do_oop_work(p); }
  void do_oop(narrowOop* p) override { do_oop_work(p); }
  // A Reference's referent and discovered fields are ordinary fields here.
  ReferenceIterationMode reference_iteration_mode() override { return DO_FIELDS; }
};

void EpoxyHeap::promote(oop obj) {
  assert(_page_allocator->is_scratch(cast_from_oop<void*>(obj)), "only scratch objects are promoted");
  if (!mark_promoted(obj)) {
    return;
  }
  GrowableArrayCHeap<oop, mtGC> stack(32);
  EpoxyPromoteClosure closure(this, &stack);
  stack.push(obj);
  while (!stack.is_empty()) {
    oop next = stack.pop();
    next->oop_iterate(&closure);
  }
}

// Applied to a copy: redirects every reference to an unpromoted object of the
// region being left to that object's copy, making the copy first if needed.
class EpoxyEvacuateClosure : public BasicOopIterateClosure {
  EpoxyHeap* const _heap;
  EpoxyPageAllocator* const _pages;
  EpoxyRegion* const _from;
  EpoxyRegion* const _to;
  GrowableArrayCHeap<oop, mtGC>* const _copies;

  template <typename T>
  void do_oop_work(T* p) {
    T element = RawAccess<>::oop_load(p);
    if (CompressedOops::is_null(element)) {
      return;
    }
    oop obj = CompressedOops::decode_not_null(element);
    if (!must_move(obj)) {
      return;
    }
    RawAccess<IS_NOT_NULL>::oop_store(p, copy(obj));
  }

public:
  EpoxyEvacuateClosure(EpoxyHeap* heap, EpoxyRegion* from, EpoxyRegion* to, GrowableArrayCHeap<oop, mtGC>* copies) :
      _heap(heap), _pages(heap->page_allocator()), _from(from), _to(to), _copies(copies) {}

  bool must_move(oop obj) const {
    void* addr = cast_from_oop<void*>(obj);
    if (!_pages->is_scratch(addr) || _pages->page_for(addr)->owner() != _from) {
      return false;
    }
    // A forwarded object has lost its header; it was unpromoted when it was copied.
    return obj->is_forwarded() || !_heap->is_promoted(obj);
  }

  // The copy of obj, made now if this is the first reference to it.
  oop copy(oop obj) {
    if (obj->is_forwarded()) {
      return obj->forwardee();
    }
    size_t size = obj->size();
    HeapWord* memory = _to->allocate(size);
    if (memory == nullptr) {
      vm_exit_out_of_memory(size * HeapWordSize, OOM_MALLOC_ERROR,
                            "Epoxy: no memory to move an object out of a scratch region");
    }
    Copy::aligned_disjoint_words(cast_from_oop<HeapWord*>(obj), memory, size);
    oop moved = cast_to_oop(memory);
    obj->forward_to(moved);
    _copies->push(moved);
    return moved;
  }

  void do_oop(oop* p) override       { do_oop_work(p); }
  void do_oop(narrowOop* p) override { do_oop_work(p); }
  ReferenceIterationMode reference_iteration_mode() override { return DO_FIELDS; }
};

oop EpoxyHeap::evacuate(EpoxyRegion* region, oop root, EpoxyRegion* target) {
  if (target == nullptr) {
    target = _global_region;
  }
  GrowableArrayCHeap<oop, mtGC> copies(32);
  EpoxyEvacuateClosure closure(this, region, target, &copies);
  if (root == nullptr || !closure.must_move(root)) {
    return root;
  }
  oop moved_root = closure.copy(root);
  while (!copies.is_empty()) {
    oop moved = copies.pop();
    moved->oop_iterate(&closure);
  }
  return moved_root;
}

EpoxyRegion* EpoxyHeap::allocation_region() const {
  Thread* thread = Thread::current();
  EpoxyRegion* scratch = EpoxyThreadLocalData::scratch(thread);
  if (scratch == nullptr || EpoxyThreadLocalData::allocate_global(thread)) {
    return _global_region;
  }
  return scratch;
}

EpoxyGlobalAllocationScope::EpoxyGlobalAllocationScope() : _thread(nullptr), _active(false) {
  if (!UseEpoxy) {
    return;
  }
  _thread = JavaThread::current();
  if (EpoxyThreadLocalData::scratch(_thread) == nullptr || EpoxyThreadLocalData::allocate_global(_thread)) {
    return;  // already allocating globally
  }
  // The thread's TLAB lies in the scratch region: give it up so that the
  // next allocation asks the heap, which now answers with the global space.
  if (UseTLAB) {
    _thread->tlab().retire();
  }
  EpoxyThreadLocalData::set_tlab_words(_thread, 0);
  EpoxyThreadLocalData::set_allocate_global(_thread, true);
  _active = true;
}

EpoxyGlobalAllocationScope::~EpoxyGlobalAllocationScope() {
  if (!_active) {
    return;
  }
  // And back: the TLAB taken meanwhile lies in the global space.
  if (UseTLAB) {
    _thread->tlab().retire();
  }
  EpoxyThreadLocalData::set_tlab_words(_thread, 0);
  EpoxyThreadLocalData::set_allocate_global(_thread, false);
}

uint EpoxyHeap::scratch_depth(Thread* thread) {
  EpoxyRegion* scratch = EpoxyThreadLocalData::scratch(thread);
  return scratch == nullptr ? 0 : scratch->depth() + 1;
}

// Pages kept from a released region that are not worth recycling (a list linked by next()).
void EpoxyHeap::adopt(EpoxyPage* kept) {
  if (kept == nullptr) {
    return;
  }
  EpoxyPage* last = kept;
  while (last->next() != nullptr) {
    last = last->next();
  }
  MutexLocker ml(&_adopted_lock, Mutex::_no_safepoint_check_flag);
  last->set_next(_adopted_pages);
  _adopted_pages = kept;
}

bool EpoxyHeap::enter_scratch(JavaThread* thread) {
  assert(thread == JavaThread::current(), "a thread enters its own regions");
  // Only code with the store barrier may run while a region is open. The
  // interpreter, C1 and C2 have it; a JVMCI compiler does not.
#if INCLUDE_JVMCI
  if (UseJVMCICompiler) {
    return false;
  }
#endif
  // Regions belong to platform threads. A virtual thread can move between
  // carriers, and freezing it copies its frames into a heap object without
  // going through the store barrier.
  if (thread->is_vthread_mounted()) {
    return false;
  }
  // Nor may a carrier thread open one: the scheduler's tasks would mount
  // virtual threads inside it, and those would then allocate in the carrier's
  // region and keep what they allocated after it is released.
  oop thread_obj = thread->threadObj();
  if (thread_obj != nullptr && thread_obj->klass()->name()->equals("jdk/internal/misc/CarrierThread")) {
    return false;
  }
  // Allocation after this point must come from the new region.
  if (UseTLAB) {
    thread->tlab().retire();
  }
  EpoxyRegion* region = new EpoxyRegion(_page_allocator, "scratch", true /* scratch */);
  region->set_parent(EpoxyThreadLocalData::scratch(thread));
  EpoxyThreadLocalData::set_scratch(thread, region);
  EpoxyThreadLocalData::set_tlab_words(thread, 0);
  return true;
}

void EpoxyHeap::exit_scratch(JavaThread* thread, address verify_skip_low, address verify_skip_high) {
  assert(thread == JavaThread::current(), "a thread leaves its own regions");
  EpoxyRegion* region = EpoxyThreadLocalData::scratch(thread);
  assert(region != nullptr, "not in a scratch region");

#ifdef ASSERT
  if (EpoxyVerifyScratchExit) {
    EpoxyVerifier::verify_release(thread, region, verify_skip_low, verify_skip_high);
  }
#endif

  // The TLAB lies in a page that is about to go.
  if (UseTLAB) {
    thread->tlab().retire();
  }
  EpoxyThreadLocalData::set_scratch(thread, region->parent());
  EpoxyThreadLocalData::set_tlab_words(thread, 0);

  EpoxyPage* kept = nullptr;
  region->release(&kept);
  adopt(kept);
  delete region;
}

void EpoxyHeap::exit_all_scratch(Thread* thread) {
  // A thread that ends inside regions gives them all up. Its TLAB has been
  // retired by the thread exit path already.
  EpoxyRegion* region = EpoxyThreadLocalData::scratch(thread);
  while (region != nullptr) {
    EpoxyRegion* parent = region->parent();
    EpoxyPage* kept = nullptr;
    region->release(&kept);
    adopt(kept);
    delete region;
    region = parent;
  }
  EpoxyThreadLocalData::set_scratch(thread, nullptr);
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
  if (_page_allocator == nullptr) {
    return false;  // asked before the heap is set up, for example while reporting an early crash
  }
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

  EpoxyRegion* region = allocation_region();
  HeapWord* res = region->allocate(size);
  if (res == nullptr) {
    return nullptr;
  }
  if (EpoxyProfile && region == _global_region) {
    EpoxyProfiler::note_global(Thread::current(), size);
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
  // A thread starts with the smallest TLAB and is given twice as much each
  // time it comes back, up to one page. Threads that allocate little, or do
  // not live long, then cost little heap.
  Thread* thread = Thread::current();
  size_t grown = EpoxyThreadLocalData::tlab_words(thread) * 2;
  size_t size = clamp(MIN2(requested_size, grown), min_size, _max_tlab_size);
  size = align_up(size, MinObjAlignment);

  assert(min_size <= size, "Size honors min size: %zu <= %zu", min_size, size);
  assert(size <= _max_tlab_size, "Size honors max size: %zu <= %zu", size, _max_tlab_size);

  if (log_is_enabled(Trace, gc)) {
    ResourceMark rm;
    log_trace(gc)("TLAB size for \"%s\" (Requested: %zuK, Min: %zuK, Max: %zuK) -> %zuK",
                  thread->name(),
                  requested_size * HeapWordSize / K,
                  min_size * HeapWordSize / K,
                  _max_tlab_size * HeapWordSize / K,
                  size * HeapWordSize / K);
  }

  // Take the rest of the current page if it can hold at least min_size.
  EpoxyRegion* region = allocation_region();
  HeapWord* res = region->allocate(align_up(min_size, MinObjAlignment), size, actual_size);
  if (res != nullptr) {
    EpoxyThreadLocalData::set_tlab_words(thread, size);
    if (EpoxyProfile && region == _global_region) {
      EpoxyProfiler::note_global(thread, *actual_size);
    }
  }
  return res;
}

HeapWord* EpoxyHeap::mem_allocate(size_t size) {
  return allocate_work(size);
}

HeapWord* EpoxyHeap::allocate_loaded_archive_space(size_t size) {
  // Archived objects live for the whole run.
  return _global_region->allocate(size);
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

// Visits the promoted objects that earlier regions left in a page: the ones
// whose first line is kept. A marked object on a line that belongs to a live
// region lies in one of that region's spans and is visited there.
void EpoxyHeap::kept_objects_iterate(EpoxyPage* page, ObjectClosure* cl) {
  HeapWord* top = page->top();
  HeapWord* addr = _promoted_marks.get_next_marked_addr(page->bottom(), top);
  while (addr < top) {
    oop obj = cast_to_oop(addr);
    size_t size = obj->size();
    if (_page_allocator->line_state(addr) == EpoxyLineKept) {
      cl->do_object(obj);
    }
    addr = _promoted_marks.get_next_marked_addr(addr + size, top);
  }
}

// Walks every scratch region of one thread: what each allocated in, and the
// kept objects of the recycled pages it is working on.
class EpoxyScratchIterateClosure : public ThreadClosure {
  EpoxyHeap* _heap;
  ObjectClosure* _cl;
public:
  EpoxyScratchIterateClosure(EpoxyHeap* heap, ObjectClosure* cl) : _heap(heap), _cl(cl) {}
  void do_thread(Thread* thread) override {
    for (EpoxyRegion* r = EpoxyThreadLocalData::scratch(thread); r != nullptr; r = r->parent()) {
      r->object_iterate(_cl);
      for (EpoxyPage* page = r->first_page(); page != nullptr; page = page->next()) {
        if (page->is_recycled()) {
          _heap->kept_objects_iterate(page, _cl);
        }
      }
    }
  }
};

void EpoxyHeap::object_iterate(ObjectClosure *cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "the heap is walked only at a safepoint");
  _global_region->object_iterate(cl);
  EpoxyScratchIterateClosure scratch(this, cl);
  Threads::java_threads_do(&scratch);

  // Pages kept from released regions hold dead memory next to the promoted
  // objects. Only the promoted ones are visited.
  for (EpoxyPage* page = _adopted_pages; page != nullptr; page = page->next()) {
    kept_objects_iterate(page, cl);
  }
  for (int b = 0; b < _page_allocator->recyclable_buckets(); b++) {
    for (EpoxyPage* page = _page_allocator->recyclable_pages(b); page != nullptr; page = page->next()) {
      kept_objects_iterate(page, cl);
    }
  }
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
  if (_page_allocator != nullptr) {
    _page_allocator->log_stats(true /* with_small_holes */);
  }
  if (EpoxyProfile) {
    EpoxyProfiler::print();
  }
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
