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

#ifndef SHARE_GC_EPOXY_EPOXYBARRIERSET_INLINE_HPP
#define SHARE_GC_EPOXY_EPOXYBARRIERSET_INLINE_HPP

#include "gc/epoxy/epoxyBarrierSet.hpp"

#include "gc/epoxy/epoxyHeap.hpp"
#include "gc/epoxy/epoxyPageAllocator.hpp"
#include "gc/shared/barrierSet.inline.hpp"
#include "oops/arrayOop.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"

inline void EpoxyBarrierSet::on_store(oop value, const void* addr, oop holder) {
  if (value == nullptr) {
    return;
  }
  EpoxyHeap* heap = EpoxyHeap::heap();
  if (!heap->page_allocator()->is_scratch(cast_from_oop<void*>(value))) {
    return;  // not an object that can be freed with a region
  }
  if (heap->is_promoted(value)) {
    return;
  }
#ifdef ASSERT
  if (!EpoxyStoreBarrier) {
    return;
  }
#endif
  on_store_slow(value, addr, holder);
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline OopCopyResult EpoxyBarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_arraycopy_in_heap(arrayOop src_obj, size_t src_offset_in_bytes, T* src_raw,
                      arrayOop dst_obj, size_t dst_offset_in_bytes, T* dst_raw,
                      size_t length) {
  T* src = arrayOopDesc::obj_offset_to_raw(src_obj, src_offset_in_bytes, src_raw);
  T* dst = arrayOopDesc::obj_offset_to_raw(dst_obj, dst_offset_in_bytes, dst_raw);
  for (size_t i = 0; i < length; i++) {
    T element = RawAccess<>::oop_load(src + i);
    if (!CompressedOops::is_null(element)) {
      EpoxyBarrierSet::on_store(CompressedOops::decode_not_null(element), dst, dst_obj);
    }
  }
  return Raw::oop_arraycopy_in_heap(src_obj, src_offset_in_bytes, src_raw,
                                    dst_obj, dst_offset_in_bytes, dst_raw,
                                    length);
}

#endif // SHARE_GC_EPOXY_EPOXYBARRIERSET_INLINE_HPP
