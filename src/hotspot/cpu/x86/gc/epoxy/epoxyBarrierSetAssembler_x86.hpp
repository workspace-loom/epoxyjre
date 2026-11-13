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

#ifndef CPU_X86_GC_EPOXY_EPOXYBARRIERSETASSEMBLER_X86_HPP
#define CPU_X86_GC_EPOXY_EPOXYBARRIERSETASSEMBLER_X86_HPP

#include "asm/macroAssembler.hpp"
#include "gc/shared/barrierSetAssembler.hpp"

// Store barrier for the template interpreter and for stubs that store
// references through MacroAssembler::store_heap_oop.
class EpoxyBarrierSetAssembler: public BarrierSetAssembler {
public:
  // Array copy stubs, used by compiled code: every reference about to be
  // copied is checked against the destination first.
  virtual void arraycopy_prologue(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                  Register src, Register dst, Register count);

  virtual void store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                        Address dst, Register val, Register tmp1, Register tmp2, Register tmp3);
};

#endif // CPU_X86_GC_EPOXY_EPOXYBARRIERSETASSEMBLER_X86_HPP
