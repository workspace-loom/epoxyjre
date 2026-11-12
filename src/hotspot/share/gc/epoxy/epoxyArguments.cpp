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

#include "gc/epoxy/epoxyArguments.hpp"
#include "gc/epoxy/epoxyHeap.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"

size_t EpoxyArguments::conservative_max_heap_alignment() {
  return UseLargePages ? os::large_page_size() : os::vm_page_size();
}

void EpoxyArguments::initialize() {
  GCArguments::initialize();

  assert(UseEpoxy, "Sanity");

  // Forcefully exit when OOME is detected. Nothing we can do at that point.
  if (FLAG_IS_DEFAULT(ExitOnOutOfMemoryError)) {
    FLAG_SET_DEFAULT(ExitOnOutOfMemoryError, true);
  }

  if (EpoxyMaxTLABSize < MinTLABSize) {
    log_warning(gc)("EpoxyMaxTLABSize < MinTLABSize, adjusting it to %zu", MinTLABSize);
    EpoxyMaxTLABSize = MinTLABSize;
  }

  if (!EpoxyElasticTLAB && EpoxyElasticTLABDecay) {
    log_warning(gc)("Disabling EpoxyElasticTLABDecay because EpoxyElasticTLAB is disabled");
    FLAG_SET_DEFAULT(EpoxyElasticTLABDecay, false);
  }

#ifdef COMPILER2
  // Enable loop strip mining: there are still non-GC safepoints, no need to make it worse
  if (FLAG_IS_DEFAULT(UseCountedLoopSafepoints)) {
    FLAG_SET_DEFAULT(UseCountedLoopSafepoints, true);
    if (FLAG_IS_DEFAULT(LoopStripMiningIter)) {
      FLAG_SET_DEFAULT(LoopStripMiningIter, 1000);
    }
  }
#endif
}

void EpoxyArguments::initialize_alignments() {
  size_t page_size = UseLargePages ? os::large_page_size() : os::vm_page_size();
  size_t align = MAX2(os::vm_allocation_granularity(), page_size);
  SpaceAlignment = align;
  HeapAlignment  = align;
}

CollectedHeap* EpoxyArguments::create_heap() {
  return new EpoxyHeap();
}
