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
 * @test TestTinyHeap
 * @requires vm.gc.Epoxy
 * @summary A heap of fewer than 16 pages is refused with a clear message; odd sizes are rounded
 * @library /test/lib
 * @run driver gc.epoxy.TestTinyHeap
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestTinyHeap {

  public static void main(String[] args) throws Exception {
    OutputAnalyzer out = ProcessTools.executeTestJava("-Xmx3m", "-XX:+UseEpoxy", "-version");
    out.shouldContain("Epoxy heap is too small");
    out.shouldNotHaveExitValue(0);

    out = ProcessTools.executeTestJava("-Xmx8m", "-XX:+UseEpoxy", "-XX:EpoxyPageSize=1m", "-version");
    out.shouldContain("Epoxy heap is too small");
    out.shouldNotHaveExitValue(0);

    out = ProcessTools.executeTestJava("-Xmx33m", "-XX:+UseEpoxy", "-XX:EpoxyPageSize=1m", "-version");
    out.shouldHaveExitValue(0);

    out = ProcessTools.executeTestJava("-Xmx5000k", "-XX:+UseEpoxy", "-XX:EpoxyPageSize=64k", "-version");
    out.shouldHaveExitValue(0);
  }
}
