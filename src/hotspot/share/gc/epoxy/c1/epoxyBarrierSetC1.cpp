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

#include "c1/c1_LIRGenerator.hpp"
#include "gc/epoxy/c1/epoxyBarrierSetC1.hpp"
#include "gc/epoxy/epoxyBarrierSet.hpp"
#include "utilities/macros.hpp"

#ifdef ASSERT
#define __ gen->lir(__FILE__, __LINE__)->
#else
#define __ gen->lir()->
#endif

void EpoxyBarrierSetC1::store_barrier(LIRAccess& access, LIR_Opr value) {
  // A constant is null or an object the compiler holds a handle to; such an
  // object is reachable from a native root, so it has been promoted already.
  if (value->is_constant()) {
    return;
  }
  LIRGenerator* gen = access.gen();

  // The holder goes along so the runtime can tell whether it has been
  // promoted. Copies are passed: the originals stay where the store needs them.
  LIR_Opr value_arg = gen->new_register(T_OBJECT);
  __ move(value, value_arg);
  LIR_Opr holder_arg = gen->new_register(T_OBJECT);
  LIR_Opr base = access.base().opr();
  if (base->is_valid()) {
    __ move(base, holder_arg);
  } else {
    __ move(LIR_OprFact::oopConst(nullptr), holder_arg);
  }

  BasicTypeArray signature(2);
  signature.append(T_OBJECT);
  signature.append(T_OBJECT);
  LIR_OprList* args = new LIR_OprList(2);
  args->append(value_arg);
  args->append(holder_arg);
  gen->call_runtime(&signature, args,
                    CAST_FROM_FN_PTR(address, EpoxyBarrierSet::store_barrier_entry),
                    voidType, nullptr);
}

void EpoxyBarrierSetC1::store_at_resolved(LIRAccess& access, LIR_Opr value) {
  if (access.is_oop()) {
    store_barrier(access, value);
  }
  BarrierSetC1::store_at_resolved(access, value);
}

LIR_Opr EpoxyBarrierSetC1::atomic_cmpxchg_at_resolved(LIRAccess& access, LIRItem& cmp_value, LIRItem& new_value) {
  if (access.is_oop()) {
    // Promoted whether or not the exchange succeeds.
    new_value.load_item();
    store_barrier(access, new_value.result());
  }
  return BarrierSetC1::atomic_cmpxchg_at_resolved(access, cmp_value, new_value);
}

LIR_Opr EpoxyBarrierSetC1::atomic_xchg_at_resolved(LIRAccess& access, LIRItem& value) {
  if (access.is_oop()) {
    value.load_item();
    store_barrier(access, value.result());
  }
  return BarrierSetC1::atomic_xchg_at_resolved(access, value);
}
