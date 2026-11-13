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

#include "gc/epoxy/c2/epoxyBarrierSetC2.hpp"
#include "gc/epoxy/epoxyBarrierSet.hpp"
#include "opto/callnode.hpp"
#include "opto/graphKit.hpp"
#include "opto/type.hpp"
#include "utilities/macros.hpp"

static const char* const EpoxyStoreBarrierName = "epoxy_store_barrier";

// void store_barrier_entry(oopDesc* value, oopDesc* holder)
static const TypeFunc* store_barrier_type() {
  const Type** fields = TypeTuple::fields(2);
  fields[TypeFunc::Parms + 0] = TypeInstPtr::BOTTOM;  // value: any object or null
  fields[TypeFunc::Parms + 1] = TypeInstPtr::BOTTOM;  // holder: any object or null
  const TypeTuple* domain = TypeTuple::make(TypeFunc::Parms + 2, fields);

  fields = TypeTuple::fields(0);
  const TypeTuple* range = TypeTuple::make(TypeFunc::Parms + 0, fields);

  return TypeFunc::make(domain, range);
}

void EpoxyBarrierSetC2::store_barrier(GraphKit* kit, Node* value, Node* base) const {
  if (kit->gvn().type(value) == TypePtr::NULL_PTR) {
    return;  // storing null
  }
  Node* holder = (base == nullptr || base->is_top()) ? kit->null() : base;
  kit->make_runtime_call(GraphKit::RC_LEAF,
                         store_barrier_type(),
                         CAST_FROM_FN_PTR(address, EpoxyBarrierSet::store_barrier_entry),
                         EpoxyStoreBarrierName,
                         TypeRawPtr::BOTTOM,
                         value, holder);
}

Node* EpoxyBarrierSetC2::store_at_resolved(C2Access& access, C2AccessValue& val) const {
  DecoratorSet decorators = access.decorators();
  // A store into an object allocated just before it, with no safepoint in
  // between, cannot let the value escape: the new object is in the innermost
  // region and nobody else has seen it.
  bool tightly_coupled_alloc = (decorators & C2_TIGHTLY_COUPLED_ALLOC) != 0;

  if (access.is_oop() && !tightly_coupled_alloc && access.is_parse_access()) {
    C2ParseAccess& parse_access = static_cast<C2ParseAccess&>(access);
    store_barrier(parse_access.kit(), val.node(), access.base());
  }
  // Stores made at optimization time belong to clones, which need nothing.
  return BarrierSetC2::store_at_resolved(access, val);
}

Node* EpoxyBarrierSetC2::atomic_cmpxchg_val_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                                        Node* new_val, const Type* value_type) const {
  if (access.is_oop()) {
    // Promoted whether or not the exchange succeeds.
    store_barrier(access.kit(), new_val, access.base());
  }
  return BarrierSetC2::atomic_cmpxchg_val_at_resolved(access, expected_val, new_val, value_type);
}

Node* EpoxyBarrierSetC2::atomic_cmpxchg_bool_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                                         Node* new_val, const Type* value_type) const {
  if (access.is_oop()) {
    store_barrier(access.kit(), new_val, access.base());
  }
  return BarrierSetC2::atomic_cmpxchg_bool_at_resolved(access, expected_val, new_val, value_type);
}

Node* EpoxyBarrierSetC2::atomic_xchg_at_resolved(C2AtomicParseAccess& access, Node* new_val, const Type* value_type) const {
  if (access.is_oop()) {
    store_barrier(access.kit(), new_val, access.base());
  }
  return BarrierSetC2::atomic_xchg_at_resolved(access, new_val, value_type);
}

bool EpoxyBarrierSetC2::array_copy_requires_gc_barriers(bool tightly_coupled_alloc, BasicType type, bool is_clone,
                                                        bool is_clone_instance, ArrayCopyPhase phase) const {
  return is_reference_type(type) && !is_clone;
}

bool EpoxyBarrierSetC2::is_gc_barrier_node(Node* node) const {
  if (!node->is_CallLeaf()) {
    return false;
  }
  const char* name = node->as_CallLeaf()->_name;
  return name != nullptr && strcmp(name, EpoxyStoreBarrierName) == 0;
}
