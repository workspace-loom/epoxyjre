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

#ifndef SHARE_GC_EPOXY_EPOXYSCOPES_HPP
#define SHARE_GC_EPOXY_EPOXYSCOPES_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

class frame;
class JavaThread;
class Method;
class oopDesc;

// Methods that run inside their own scratch region: one is opened when the
// method is entered and released when it returns or is left by an exception.
//
// The methods are named in the file given by -XX:EpoxyScopeFile, one per
// line as package.Class.method, optionally followed by a descriptor such as
// (I)V to pick one overload. Blank lines and lines starting with # are
// ignored. A listed method is never compiled or inlined, so only the
// interpreter's method entry and exit need to know about regions; everything
// it calls is compiled as usual.
class EpoxyScopes : public AllStatic {
public:
  // Reads the file. Called once, before any class is linked.
  static void initialize();
  static bool has_scopes();

  // Marks method if it is listed. Called when a method is linked.
  static void mark_if_listed(Method* method);

  // Called by the interpreter for a marked method.
  static void enter(JavaThread* thread, const frame& activation);
  // result is the reference being returned, or null.
  static void exit(JavaThread* thread, const frame& activation, oopDesc* result);
};

#endif // SHARE_GC_EPOXY_EPOXYSCOPES_HPP
