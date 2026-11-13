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

#ifndef SHARE_GC_EPOXY_C1_EPOXYBARRIERSETC1_HPP
#define SHARE_GC_EPOXY_C1_EPOXYBARRIERSETC1_HPP

#include "gc/shared/c1/barrierSetC1.hpp"

// The store barrier for code compiled by C1. Before a reference is stored
// into an object, the runtime is asked whether the value has to be promoted
// (EpoxyBarrierSet::store_barrier_entry).
class EpoxyBarrierSetC1 : public BarrierSetC1 {
private:
  // Emits the call. value must be a register or a constant.
  void store_barrier(LIRAccess& access, LIR_Opr value);

protected:
  virtual void store_at_resolved(LIRAccess& access, LIR_Opr value);
  virtual LIR_Opr atomic_cmpxchg_at_resolved(LIRAccess& access, LIRItem& cmp_value, LIRItem& new_value);
  virtual LIR_Opr atomic_xchg_at_resolved(LIRAccess& access, LIRItem& value);
};

#endif // SHARE_GC_EPOXY_C1_EPOXYBARRIERSETC1_HPP
