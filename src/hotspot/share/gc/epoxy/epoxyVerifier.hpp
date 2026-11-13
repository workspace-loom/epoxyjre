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

#ifndef SHARE_GC_EPOXY_EPOXYVERIFIER_HPP
#define SHARE_GC_EPOXY_EPOXYVERIFIER_HPP

#include "memory/allStatic.hpp"
#include "utilities/macros.hpp"

#ifdef ASSERT

class EpoxyRegion;
class JavaThread;

// Debug-only check that the store barrier promoted every object of a scratch
// region that is reachable from outside the region, before the region is
// released. Releasing frees every page without a promoted object without
// looking at its contents, so one missed promotion leaves a dangling
// reference.
class EpoxyVerifier : public AllStatic {
public:
  // Called by a JavaThread (in VM state, not at a safepoint) that is about to
  // release `region`, its innermost scratch region. Brings the VM to a
  // safepoint, walks the object graph from all roots, and calls fatal()
  // describing the first few violations if any reachable reference points
  // into `region` at an object for which is_promoted is false, or at memory
  // that is not in the heap.
  // [skip_low, skip_high] is a range of the releasing thread's own stack whose
  // slots are not checked: the frame of the method activation being left,
  // whose locals die with it. Both null to check everything.
  static void verify_release(JavaThread* thread, EpoxyRegion* region,
                             address skip_low = nullptr, address skip_high = nullptr);
};

#endif // ASSERT

#endif // SHARE_GC_EPOXY_EPOXYVERIFIER_HPP
