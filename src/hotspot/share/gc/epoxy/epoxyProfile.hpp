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

#ifndef SHARE_GC_EPOXY_EPOXYPROFILE_HPP
#define SHARE_GC_EPOXY_EPOXYPROFILE_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

class Klass;
class Thread;

// Counters behind -XX:+EpoxyProfile: what is promoted, by class, and which
// threads allocate in the global space, by thread name with digits removed.
// Both are memory that no region frees. Printed when the VM exits.
class EpoxyProfiler : public AllStatic {
public:
  static void note_promoted(Klass* klass, size_t words);
  static void note_global(Thread* thread, size_t words);
  static void print();
};

#endif // SHARE_GC_EPOXY_EPOXYPROFILE_HPP
