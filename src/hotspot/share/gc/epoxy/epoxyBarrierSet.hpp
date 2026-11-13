/*
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

#ifndef SHARE_GC_EPOXY_EPOXYBARRIERSET_HPP
#define SHARE_GC_EPOXY_EPOXYBARRIERSET_HPP

#include "gc/shared/barrierSet.hpp"

// Epoxy's only barrier is on reference stores. A scratch object, one that
// will be freed with its region, must not become reachable from anything that
// outlives the region. Before such a store the storing thread promotes the
// object and every scratch object it reaches (EpoxyHeap::promote), so that:
//
//   no object another thread can reach ever refers to an unpromoted scratch object.
//
// Unpromoted scratch objects are therefore private to the thread that owns
// their region, and promotion needs no synchronisation.
class EpoxyBarrierSet: public BarrierSet {
public:
  EpoxyBarrierSet();

  virtual void print_on(outputStream *st) const {}

  virtual void on_thread_create(Thread* thread);
  virtual void on_thread_destroy(Thread* thread);

  // To be called before value is stored at addr. holder is the object that
  // contains addr when the caller knows it, else null. addr outside the heap
  // is a native root.
  static inline void on_store(oop value, const void* addr, oop holder);
  static void on_store_slow(oop value, const void* addr, oop holder);

  // Entry for generated code: value is not null; holder is the object being
  // stored into, or null for a store that is not into a heap object.
  static void store_barrier_entry(oopDesc* value, oopDesc* holder);

  template <DecoratorSet decorators, typename BarrierSetT = EpoxyBarrierSet>
  class AccessBarrier: public BarrierSet::AccessBarrier<decorators, BarrierSetT> {
    typedef BarrierSet::AccessBarrier<decorators, BarrierSetT> Raw;

  public:
    template <typename T>
    static void oop_store_in_heap(T* addr, oop value) {
      EpoxyBarrierSet::on_store(value, addr, nullptr);
      Raw::oop_store(addr, value);
    }

    static void oop_store_in_heap_at(oop base, ptrdiff_t offset, oop value) {
      EpoxyBarrierSet::on_store(value, cast_from_oop<void*>(base), base);
      Raw::oop_store_at(base, offset, value);
    }

    // A compare-and-swap promotes the new value whether or not it succeeds.
    template <typename T>
    static oop oop_atomic_cmpxchg_in_heap(T* addr, oop compare_value, oop new_value) {
      EpoxyBarrierSet::on_store(new_value, addr, nullptr);
      return Raw::oop_atomic_cmpxchg(addr, compare_value, new_value);
    }

    static oop oop_atomic_cmpxchg_in_heap_at(oop base, ptrdiff_t offset, oop compare_value, oop new_value) {
      EpoxyBarrierSet::on_store(new_value, cast_from_oop<void*>(base), base);
      return Raw::oop_atomic_cmpxchg_at(base, offset, compare_value, new_value);
    }

    template <typename T>
    static oop oop_atomic_xchg_in_heap(T* addr, oop new_value) {
      EpoxyBarrierSet::on_store(new_value, addr, nullptr);
      return Raw::oop_atomic_xchg(addr, new_value);
    }

    static oop oop_atomic_xchg_in_heap_at(oop base, ptrdiff_t offset, oop new_value) {
      EpoxyBarrierSet::on_store(new_value, cast_from_oop<void*>(base), base);
      return Raw::oop_atomic_xchg_at(base, offset, new_value);
    }

    // Every element is checked against the destination before any is copied,
    // so no other thread can see an unpromoted element in the destination.
    template <typename T>
    static OopCopyResult oop_arraycopy_in_heap(arrayOop src_obj, size_t src_offset_in_bytes, T* src_raw,
                                               arrayOop dst_obj, size_t dst_offset_in_bytes, T* dst_raw,
                                               size_t length);

    // Stores outside the heap are native roots: JNI globals, class loader
    // data, the string table, monitors, a thread's own Thread object.
    template <typename T>
    static void oop_store_not_in_heap(T* addr, oop value) {
      EpoxyBarrierSet::on_store(value, addr, nullptr);
      Raw::oop_store(addr, value);
    }

    template <typename T>
    static oop oop_atomic_cmpxchg_not_in_heap(T* addr, oop compare_value, oop new_value) {
      EpoxyBarrierSet::on_store(new_value, addr, nullptr);
      return Raw::oop_atomic_cmpxchg(addr, compare_value, new_value);
    }

    template <typename T>
    static oop oop_atomic_xchg_not_in_heap(T* addr, oop new_value) {
      EpoxyBarrierSet::on_store(new_value, addr, nullptr);
      return Raw::oop_atomic_xchg(addr, new_value);
    }
  };
};

template<>
struct BarrierSet::GetName<EpoxyBarrierSet> {
  static const BarrierSet::Name value = BarrierSet::EpoxyBarrierSet;
};

template<>
struct BarrierSet::GetType<BarrierSet::EpoxyBarrierSet> {
  typedef ::EpoxyBarrierSet type;
};

#endif // SHARE_GC_EPOXY_EPOXYBARRIERSET_HPP
