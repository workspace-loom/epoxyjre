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
 * @test TestScratchThreadDiesInRegion
 * @requires vm.gc.Epoxy & vm.debug
 * @summary A thread that ends inside scratch regions gives them up; what it let escape survives and is still seen by heap inspection
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit gc.epoxy.TestScratchThreadDiesInRegion
 */

import jdk.test.lib.dcmd.PidJcmdExecutor;
import jdk.test.lib.process.OutputAnalyzer;

public class TestScratchThreadDiesInRegion extends ScratchTestSupport {

  static class Marker {
    int id;
    Marker(int id) { this.id = id; }
  }

  static final int COUNT = 5000;
  static volatile Marker[] markers;
  static volatile Graph graph;
  static volatile Throwable failed;

  static void body(boolean real) {
    try {
      if (real) {
        enter();
        enter();                       // dies two regions deep
      }
      Marker[] m = new Marker[COUNT];
      for (int i = 0; i < COUNT; i++) {
        m[i] = new Marker(i);
      }
      markers = m;
      graph = buildGraph(21);
      garbage(8, 1000);                // unescaped garbage in the same regions
    } catch (Throwable e) {
      failed = e;
    }
  }

  static void verify(String what) {
    Marker[] m = markers;
    check(m.length == COUNT, what + ": marker array length");
    for (int i = 0; i < COUNT; i++) {
      check(m[i].id == i, what + ": marker " + i);
    }
    verifyGraph(graph, 21, what);
  }

  public static void main(String[] args) throws Exception {
    warmUp();
    body(false);                       // warm up outside any region
    markers = null;
    graph = null;

    for (int round = 0; round < 20; round++) {
      Thread t = new Thread(() -> body(true));
      t.start();
      t.join();
      if (failed != null) throw new IllegalStateException(failed);
      churn();
      verify("round " + round);
    }
    promoted(graph, "graph escaped by a thread that died in its region");

    // Heap inspection must still see every marker of the last round, and may
    // see earlier rounds' markers only if they are still marked (they are:
    // promoted objects are never unmarked), so the count is at least COUNT.
    OutputAnalyzer out = new PidJcmdExecutor().execute("GC.class_histogram");
    out.shouldHaveExitValue(0);
    String line = out.asLines().stream()
        .filter(l -> l.contains("gc.epoxy.TestScratchThreadDiesInRegion$Marker") && !l.contains("[L"))
        .findFirst()
        .orElseThrow(() -> new IllegalStateException("Marker not in histogram"));
    int count = Integer.parseInt(line.trim().split("\\s+")[1]);
    check(count >= COUNT, "histogram shows " + count + " markers, expected at least " + COUNT);
    verify("after histogram");
  }
}
