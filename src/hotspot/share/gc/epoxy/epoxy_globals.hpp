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
