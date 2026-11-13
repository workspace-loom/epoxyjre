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

#ifndef SHARE_GC_EPOXY_C2_EPOXYBARRIERSETC2_HPP
#define SHARE_GC_EPOXY_C2_EPOXYBARRIERSETC2_HPP

#include "gc/shared/c2/barrierSetC2.hpp"

// The store barrier for code compiled by C2. Before a reference is stored
// into an object, a leaf call asks the runtime whether the value has to be
// promoted (EpoxyBarrierSet::store_barrier_entry). The call takes the whole
// memory state as input, so every earlier store to the value's own fields is
// done before the runtime walks them, and it changes no Java-visible memory.
class EpoxyBarrierSetC2 : public BarrierSetC2 {
private:
  void store_barrier(GraphKit* kit, Node* value, Node* base) const;

protected:
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const;
  virtual Node* atomic_cmpxchg_val_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                               Node* new_val, const Type* value_type) const;
  virtual Node* atomic_cmpxchg_bool_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                                Node* new_val, const Type* value_type) const;
  virtual Node* atomic_xchg_at_resolved(C2AtomicParseAccess& access, Node* new_val, const Type* value_type) const;

public:
  // Reference arrays are copied by the stubs, which check every element
  // against the destination first. A clone needs nothing: the copy is a new
  // object in the innermost region.
  virtual bool array_copy_requires_gc_barriers(bool tightly_coupled_alloc, BasicType type, bool is_clone,
                                               bool is_clone_instance, ArrayCopyPhase phase) const;

  // Lets escape analysis accept the barrier call's object arguments.
  virtual bool is_gc_barrier_node(Node* node) const;
};

#endif // SHARE_GC_EPOXY_C2_EPOXYBARRIERSETC2_HPP
