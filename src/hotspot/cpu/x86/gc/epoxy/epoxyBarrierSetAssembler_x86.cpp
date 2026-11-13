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

#include "asm/macroAssembler.inline.hpp"
#include "gc/epoxy/epoxyBarrierSet.hpp"
#include "gc/epoxy/epoxyBarrierSetAssembler.hpp"
#include "gc/epoxy/epoxyHeap.hpp"
#include "gc/epoxy/epoxyPageAllocator.hpp"
#include "runtime/sharedRuntime.hpp"

#define __ masm->

void EpoxyBarrierSetAssembler::arraycopy_prologue(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                                  Register src, Register dst, Register count) {
  if (!is_reference_type(type)) {
    return;
  }
  // The three values may be in argument registers in any order, so they go
  // through the stack.
  __ push_call_clobbered_registers();
  __ push(src);
  __ push(dst);
  __ push(count);
  __ pop(c_rarg2);
  __ pop(c_rarg1);
  __ pop(c_rarg0);
  __ movl(c_rarg2, c_rarg2);   // the count is a non-negative 32-bit value: clear the upper half
  __ MacroAssembler::call_VM_leaf_base(CAST_FROM_FN_PTR(address, EpoxyBarrierSet::arraycopy_barrier_entry), 3);
  __ pop_call_clobbered_registers();
}

void EpoxyBarrierSetAssembler::store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                        Address dst, Register val, Register tmp1, Register tmp2, Register tmp3) {
  // val == noreg stores null, which needs no barrier.
  if (is_reference_type(type) && val != noreg) {
    bool in_heap = (decorators & IN_HEAP) != 0;
    Register holder = in_heap ? dst.base() : noreg;
    EpoxyPageAllocator* pages = EpoxyHeap::heap()->page_allocator();
    Label done;

    // val is an uncompressed reference here. Skip the call unless it points
    // into a scratch page: one unsigned range check and one byte load.
    __ testptr(val, val);
    __ jcc(Assembler::zero, done);

    // The heap bounds and the table never change once the heap exists, and
    // code is generated after that, so they are embedded as constants.
    assert_different_registers(val, rscratch1);
    assert(dst.base() != rscratch1 && dst.index() != rscratch1, "rscratch1 is used by the barrier");
    Label not_scratch;
    __ push(rax);
    __ movptr(rax, val);
    __ mov64(rscratch1, (int64_t)(intptr_t)pages->reserved().start());
    __ subptr(rax, rscratch1);
    __ mov64(rscratch1, (int64_t)pages->reserved().byte_size());
    __ cmpptr(rax, rscratch1);
    __ jcc(Assembler::aboveEqual, not_scratch);
    __ shrptr(rax, pages->page_shift());
    __ mov64(rscratch1, (int64_t)(intptr_t)pages->kind_table());
    __ cmpb(Address(rscratch1, rax, Address::times_1), 0);
    __ jcc(Assembler::equal, not_scratch);
    __ pop(rax);

    // Slow path: let the runtime decide. Every register the caller may be
    // using is preserved; the two arguments go through the stack because val
    // and holder may themselves be argument registers.
    __ push_call_clobbered_registers();
    __ push(val);
    if (holder != noreg) {
      __ push(holder);
    } else {
      __ push(NULL_WORD);
    }
    __ pop(c_rarg1);
    __ pop(c_rarg0);
    __ MacroAssembler::call_VM_leaf_base(CAST_FROM_FN_PTR(address, EpoxyBarrierSet::store_barrier_entry), 2);
    __ pop_call_clobbered_registers();
    __ jmp(done);

    __ bind(not_scratch);
    __ pop(rax);
    __ bind(done);
  }
  BarrierSetAssembler::store_at(masm, decorators, type, dst, val, tmp1, tmp2, tmp3);
}
