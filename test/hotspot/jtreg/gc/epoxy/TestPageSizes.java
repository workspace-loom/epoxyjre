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
 */

package gc.epoxy;

/**
 * @test TestPageSizes
 * @requires vm.gc.Epoxy
 * @summary The heap works at every supported page size and rejects unsupported ones
 * @library /test/lib
 * @build gc.epoxy.TestLargeObjects
 * @run driver gc.epoxy.TestPageSizes
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestPageSizes {

  static void passWith(String pageSize) throws Exception {
    OutputAnalyzer out = ProcessTools.executeTestJava(
        "-Xmx256m", "-XX:+UseEpoxy", "-XX:EpoxyPageSize=" + pageSize, "gc.epoxy.TestLargeObjects");
    out.shouldHaveExitValue(0);
  }

  static void failWith(String pageSize) throws Exception {
    OutputAnalyzer out = ProcessTools.executeTestJava(
        "-Xmx256m", "-XX:+UseEpoxy", "-XX:EpoxyPageSize=" + pageSize, "-version");
    out.shouldContain("EpoxyPageSize");
    out.shouldNotHaveExitValue(0);
  }

  public static void main(String[] args) throws Exception {
    passWith("64k");
    passWith("128k");
    passWith("256k");
    passWith("1m");
    failWith("100k");   // not a power of two
    failWith("32k");    // below the range
    failWith("2m");     // above the range
  }
}
