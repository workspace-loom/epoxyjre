/*
 * Copyright (c) 2020, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_EPOXY_EPOXY_GLOBALS_HPP
#define SHARE_GC_EPOXY_EPOXY_GLOBALS_HPP

#include "runtime/globals_shared.hpp"

//
// Defines all globals flags used by the Epoxy.
//

#define GC_EPOXY_FLAGS(develop,                                           \
                         develop_pd,                                        \
                         product,                                           \
                         product_pd,                                        \
                         range,                                             \
                         constraint)                                        \
                                                                            \
  product(size_t, EpoxyPageSize, 256 * K,                                   \
          "Size in bytes of one Epoxy heap page. Must be a power of two "   \
          "between 64K and 1M.")                                            \
          range(64 * K, 1 * M)                                              \
                                                                            \
  product(bool, EpoxyProfile, false,                                        \
          "Count promoted objects by class and global-space allocation by " \
          "thread, and print both when the VM exits.")                      \
                                                                            \
  product(uintx, EpoxyProfileDelay, 0,                                      \
          "Seconds after VM start before EpoxyProfile starts counting.")    \
                                                                            \
  product(ccstr, EpoxyScopeFile, nullptr,                                   \
          "File naming the methods that run inside their own scratch "      \
          "region, one per line as package.Class.method with an optional "  \
          "descriptor.")                                                    \
                                                                            \
  develop(bool, EpoxyStoreBarrier, true,                                    \
          "Promote objects that escape a scratch region. Turning this "     \
          "off makes the VM unsafe; it exists so that tests can show the "  \
          "verifier catching a missing promotion.")                         \
                                                                            \
  develop(bool, EpoxyVerifyScratchExit, false,                              \
          "When a scratch region is released, walk the heap from the "      \
          "roots and fail if anything reachable is about to be freed. "     \
          "Test tooling; not present in product builds.")                   \
                                                                            \
  product(size_t, EpoxyMinRecycleLines, 2,                                  \
          "A page kept by a released scratch region goes on the "           \
          "recyclable list if it has a hole (a run of free lines) of at "   \
          "least this many 64-byte lines.")                                 \
          range(1, 1024)                                                    \
                                                                            \
  product(size_t, EpoxyMaxRecycleBytes, 64 * K,                             \
          "A scratch region allocates requests of at most this many bytes " \
          "in the holes of recycled pages; bigger ones go to fresh pages.") \
          range(256, 64 * K)                                                \
                                                                            \
  product(size_t, EpoxyPrintHeapSteps, 20,                                  \
          "Print heap occupancy stats with this number of steps. "          \
          "0 turns the printing off.")                                      \
          range(0, max_intx)                                                \
                                                                            \
  product(size_t, EpoxyUpdateCountersStep, 1 * M,                           \
          "Update heap occupancy counters after allocating this much "      \
          "memory. Higher values would make allocations faster at "         \
          "the expense of lower resolution in heap counters.")              \
          range(1, max_intx)                                                \

// end of GC_EPOXY_FLAGS

#endif // SHARE_GC_EPOXY_EPOXY_GLOBALS_HPP
