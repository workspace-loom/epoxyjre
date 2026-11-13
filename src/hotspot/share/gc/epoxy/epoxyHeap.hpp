/*
 * Copyright (c) 2023, 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_EPOXY_EPOXYHEAP_HPP
#define SHARE_GC_EPOXY_EPOXYHEAP_HPP

#include "gc/epoxy/epoxyBarrierSet.hpp"
#include "gc/epoxy/epoxyMonitoringSupport.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "memory/allocation.hpp"
#include "gc/shared/markBitMap.hpp"
#include "runtime/atomic.hpp"
#include "runtime/mutex.hpp"
#include "services/memoryManager.hpp"

class EpoxyPage;
class EpoxyPageAllocator;
class EpoxyRegion;

class EpoxyHeap : public CollectedHeap {
  friend class VMStructs;
private:
  EpoxyMonitoringSupport* _monitoring_support;
  MemoryPool* _pool;
  GCMemoryManager _memory_manager;
  EpoxyPageAllocator* _page_allocator;
  EpoxyRegion* _global_region;
  EpoxyPage* _adopted_pages;     // pages kept from released scratch regions
  MarkBitMap _promoted_marks;    // one bit per object start: set when the object is promoted
  Mutex _adopted_lock;
  size_t _page_size_bytes;
  size_t _max_tlab_size;
  size_t _step_counter_update;
  size_t _step_heap_print;
  Atomic<size_t> _last_counter_update;
  Atomic<size_t> _last_heap_print;

  static const size_t MinPages = 16;

  void print_tracing_info() const override;
  void stop() override {};

public:
  static EpoxyHeap* heap();

  EpoxyHeap() :
          _memory_manager("Epoxy Heap"),
          _page_allocator(nullptr),
          _global_region(nullptr),
          _adopted_pages(nullptr),
          _adopted_lock(Mutex::nosafepoint, "EpoxyAdopted_lock"),
          _page_size_bytes(0) {};

  Name kind() const override {
    return CollectedHeap::Epoxy;
  }

  const char* name() const override {
    return "Epoxy";
  }

  jint initialize() override;
  void initialize_serviceability() override;
  void post_initialize() override;

  GrowableArray<GCMemoryManager*> memory_managers() override;
  GrowableArray<MemoryPool*> memory_pools() override;

  EpoxyPageAllocator* page_allocator() const { return _page_allocator; }
  EpoxyRegion* global_region() const         { return _global_region; }

  // Scratch regions. A thread inside one allocates from it; leaving it frees
  // every page that holds no promoted object. Entering is refused (false)
  // while compiled code could run, because only the interpreter and the VM's
  // own stores have the barrier that promotes escaping objects.
  bool enter_scratch(JavaThread* thread);
  void exit_scratch(JavaThread* thread);
  void exit_all_scratch(Thread* thread);
  static uint scratch_depth(Thread* thread);

  // Promotion. A promoted object survives the release of its scratch region.
  bool is_promoted(oop obj) const;
  const MarkBitMap* promoted_marks() const { return &_promoted_marks; }
  // Promotes obj and every scratch object reachable from it. obj must be an
  // unpromoted scratch object of the calling thread.
  void promote(oop obj);
  // Marks one object; false if it was promoted already.
  bool mark_promoted(oop obj);

  // The region the calling thread allocates in.
  EpoxyRegion* allocation_region() const;

  // Sizes are in whole pages: a page counts as used once a region owns it.
  size_t max_capacity() const override { return _reserved.byte_size(); }
  size_t capacity()     const override;
  size_t used()         const override;

  // used() and capacity() change under allocating threads without Heap_lock.
  // These read used first: committed memory only grows and is raised before
  // a page is counted as used, so that order can never show used > committed.
  size_t unused() const override;
  MemoryUsage memory_usage() override;

  bool is_in(const void* p) const override;

  bool requires_barriers(stackChunkOop obj) const override { return false; }

  // Allocation
  HeapWord* allocate_work(size_t size);
  HeapWord* mem_allocate(size_t size) override;
  HeapWord* allocate_new_tlab(size_t min_size,
                              size_t requested_size,
                              size_t* actual_size) override;

  // TLAB allocation
  // Not the committed size: that is near zero at start-up and would give the
  // first threads the smallest possible TLAB for good.
  size_t tlab_capacity()         const override { return max_capacity(); }
  size_t tlab_used()             const override { return used();         }
  size_t max_tlab_size()         const override { return _max_tlab_size; }
  size_t unsafe_max_tlab_alloc() const override;

  void collect(GCCause::Cause cause) override;
  void do_full_collection(bool clear_all_soft_refs) override;

  // Heap walking support
  void object_iterate(ObjectClosure* cl) override;

  // Object pinning support: every object is implicitly pinned
  void pin_object(JavaThread* thread, oop obj) override { }
  void unpin_object(JavaThread* thread, oop obj) override { }

  // No support for block parsing.
  HeapWord* block_start(const void* addr) const { return nullptr;  }
  bool block_is_obj(const HeapWord* addr) const { return false; }

  // No GC threads
  void gc_threads_do(ThreadClosure* tc) const override {}

  // No nmethod handling
  void register_nmethod(nmethod* nm) override {}
  void unregister_nmethod(nmethod* nm) override {}
  void verify_nmethod(nmethod* nm) override {}

  // No heap verification
  void prepare_for_verify() override {}
  void verify(VerifyOption option) override {}

  MemRegion reserved_region() const { return _reserved; }
  bool is_in_reserved(const void* addr) const { return _reserved.contains(addr); }

  // Support for loading objects from CDS archive into the heap
  bool can_load_archived_objects() const override { return true; }
  HeapWord* allocate_loaded_archive_space(size_t size) override;

  void print_heap_on(outputStream* st) const override;
  void print_gc_on(outputStream* st) const override {}
  bool print_location(outputStream* st, void* addr) const override;

private:
  void print_heap_info(size_t used) const;
  void print_metaspace_info() const;

};

// While one of these is live, the current thread allocates in the global
// space even if it is inside a scratch region.
//
// Needed where the VM creates an object whose real allocation point lies in
// the past. The case that matters: C2 can remove an allocation altogether
// (scalar replacement), and when the compiled code is later deoptimized the
// VM builds the object then. By that time the thread may have entered a
// region younger than the one the object logically belongs to; the object
// would be freed with the inner region while outer code still uses it. Its
// true region is not known at that point, so it goes where it can never be
// freed too early.
class EpoxyGlobalAllocationScope : public StackObj {
  JavaThread* _thread;
  bool        _active;
public:
  EpoxyGlobalAllocationScope();
  ~EpoxyGlobalAllocationScope();
};

#endif // SHARE_GC_EPOXY_EPOXYHEAP_HPP
