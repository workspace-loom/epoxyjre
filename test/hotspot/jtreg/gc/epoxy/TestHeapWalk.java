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
 * @test TestHeapWalk
 * @requires vm.gc.Epoxy
 * @summary Heap inspection walks every object when pages are only partly filled
 * @library /test/lib
 * @run main/othervm -Xmx128m -XX:+UseEpoxy gc.epoxy.TestHeapWalk
 * @run main/othervm -Xmx128m -XX:+UseEpoxy -XX:-UseTLAB gc.epoxy.TestHeapWalk
 * @run main/othervm -Xmx128m -XX:+UseEpoxy -XX:EpoxyPageSize=64k gc.epoxy.TestHeapWalk
 */

import jdk.test.lib.dcmd.PidJcmdExecutor;
import jdk.test.lib.process.OutputAnalyzer;

public class TestHeapWalk {

  static class Marker {
    int id;
    Marker(int id) { this.id = id; }
  }

  static final int COUNT = 50_000;

  public static void main(String[] args) throws Exception {
    Marker[] markers = new Marker[COUNT];
    for (int i = 0; i < COUNT; i++) {
      markers[i] = new Marker(i);
      if (i % 100 == 0) {
        // Interleave larger allocations so page tails are left partly used.
        byte[] pad = new byte[40_000];
        pad[0] = 1;
      }
    }

    OutputAnalyzer out = new PidJcmdExecutor().execute("GC.class_histogram");
    out.shouldHaveExitValue(0);
    String line = out.asLines().stream()
        .filter(l -> l.contains("gc.epoxy.TestHeapWalk$Marker") && !l.contains("[L"))
        .findFirst()
        .orElseThrow(() -> new IllegalStateException("Marker not in histogram:\n" + out.getOutput()));
    // Histogram line: "  num:  #instances  #bytes  class name"
    String[] cols = line.trim().split("\\s+");
    if (Integer.parseInt(cols[1]) != COUNT) {
      throw new IllegalStateException("Expected " + COUNT + " Marker instances, histogram says: " + line);
    }

    for (int i = 0; i < COUNT; i++) {
      if (markers[i].id != i) {
        throw new IllegalStateException("Marker " + i + " corrupted");
      }
    }
  }
}
