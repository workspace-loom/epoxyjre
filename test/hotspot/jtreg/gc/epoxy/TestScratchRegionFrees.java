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
 * @test TestScratchRegionFrees
 * @requires vm.gc.Epoxy
 * @summary Garbage allocated inside a scratch region is given back when the region is left
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx128m gc.epoxy.TestScratchRegionFrees
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx128m -XX:-UseTLAB gc.epoxy.TestScratchRegionFrees
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx128m -XX:EpoxyPageSize=64k gc.epoxy.TestScratchRegionFrees
 */

import jdk.test.whitebox.WhiteBox;

public class TestScratchRegionFrees {

  static final WhiteBox WB = WhiteBox.getWhiteBox();

  // Allocates about megabytes MB that nothing refers to afterwards. Returns nothing,
  // so no local of the caller can refer into the region.
  static void garbage(int megabytes) {
    long sum = 0;
    for (int i = 0; i < megabytes * 16; i++) {
      byte[] b = new byte[64 * 1024 - 32];
      b[i % b.length] = 1;
      sum += b.length;
      int[] small = new int[4];
      small[0] = i;
      sum += small[0];
    }
    if (sum == 0) throw new IllegalStateException();
  }

  static void bigGarbage() {
    long[] big = new long[1_000_000];   // 8 MB: a run of pages to itself
    big[big.length - 1] = 42;
    if (big[big.length - 1] != 42) throw new IllegalStateException();
  }

  public static void main(String[] args) {
    garbage(1);       // warm up: load and resolve everything the helpers use
    bigGarbage();

    if (WB.epoxyScratchDepth() != 0) throw new IllegalStateException("depth before");

    // 200 rounds of 20 MB is 4 GB through a 128 MB heap.
    for (int round = 0; round < 200; round++) {
      if (!WB.epoxyEnterScratch()) throw new IllegalStateException("could not enter a scratch region");
      if (WB.epoxyScratchDepth() != 1) throw new IllegalStateException("depth inside");
      garbage(20);
      WB.epoxyExitScratch();
      if (WB.epoxyScratchDepth() != 0) throw new IllegalStateException("depth after");
    }

    // Large objects are given back too: 100 rounds of 8 MB.
    for (int round = 0; round < 100; round++) {
      WB.epoxyEnterScratch();
      bigGarbage();
      WB.epoxyExitScratch();
    }

    // Nested regions: the inner one is freed on its own exit.
    WB.epoxyEnterScratch();
    for (int round = 0; round < 100; round++) {
      WB.epoxyEnterScratch();
      if (WB.epoxyScratchDepth() != 2) throw new IllegalStateException("nested depth");
      garbage(10);
      WB.epoxyExitScratch();
    }
    garbage(10);
    WB.epoxyExitScratch();
    if (WB.epoxyScratchDepth() != 0) throw new IllegalStateException("depth at end");
  }
}
