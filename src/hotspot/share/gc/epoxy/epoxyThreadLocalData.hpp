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
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_GC_EPOXY_EPOXYTHREADLOCALDATA_HPP
#define SHARE_GC_EPOXY_EPOXYTHREADLOCALDATA_HPP

#include "gc/shared/gc_globals.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/debug.hpp"

// Per-thread state kept in the thread's GC data area.
class EpoxyThreadLocalData {
private:
  // Size in words of the largest TLAB this thread has been given. A thread
  // starts with the smallest TLAB and is given twice as much each time it
  // comes back for more, so a short-lived thread costs little heap.
  size_t _tlab_words;

  EpoxyThreadLocalData() : _tlab_words(0) {}

  static EpoxyThreadLocalData* data(Thread* thread) {
    assert(UseEpoxy, "Sanity");
    return thread->gc_data<EpoxyThreadLocalData>();
  }

public:
  static void create(Thread* thread) {
    new (data(thread)) EpoxyThreadLocalData();
  }

  static void destroy(Thread* thread) {
    data(thread)->~EpoxyThreadLocalData();
  }

  static size_t tlab_words(Thread* thread)                { return data(thread)->_tlab_words; }
  static void set_tlab_words(Thread* thread, size_t words) { data(thread)->_tlab_words = words; }
};

#endif // SHARE_GC_EPOXY_EPOXYTHREADLOCALDATA_HPP
