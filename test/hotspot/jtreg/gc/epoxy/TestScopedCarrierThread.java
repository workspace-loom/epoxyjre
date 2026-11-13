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
 * @test TestScopedCarrierThread
 * @requires vm.gc.Epoxy & vm.debug & vm.continuations
 * @summary A scope on the method that runs fork-join tasks must not put virtual threads, which the scheduler mounts through it, in a carrier's region
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx512m
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-carrier.txt gc.epoxy.TestScopedCarrierThread
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx512m -XX:+EpoxyVerifyScratchExit -Dthreads=20 -Drounds=5
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-carrier.txt gc.epoxy.TestScopedCarrierThread
 */

import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.atomic.AtomicInteger;

public class TestScopedCarrierThread extends ScratchTestSupport {
  static final int THREADS = Integer.getInteger("threads", 100);
  static final int ROUNDS = Integer.getInteger("rounds", 20);
  static final AtomicInteger failures = new AtomicInteger();
  static final AtomicInteger scopedTasks = new AtomicInteger();

  static void garbage() {
    int sum = 0;
    for (int k = 0; k < 200; k++) {
      int[] a = new int[32];
      a[k % 32] = k;
      sum += a[k % 32];
    }
    if (sum < 0) throw new AssertionError();
  }

  // Virtual threads run outside any region, so their garbage is never freed:
  // the default load is about 120 MB.
  // Each virtual thread keeps arrays in locals across yields: its frames are
  // copied to the heap while it is unmounted and it may come back on another carrier.
  static void body(int id) {
    try {
      for (int r = 0; r < ROUNDS; r++) {
        int[] mine = new int[64];
        for (int j = 0; j < mine.length; j++) mine[j] = id * 1000 + r;
        String text = "vt-" + id + "-" + r;
        garbage();
        Thread.sleep(1);
        garbage();
        Thread.yield();
        for (int j = 0; j < mine.length; j++) {
          if (mine[j] != id * 1000 + r) { failures.incrementAndGet(); return; }
        }
        if (!text.equals("vt-" + id + "-" + r)) { failures.incrementAndGet(); return; }
      }
    } catch (Throwable t) {
      t.printStackTrace();
      failures.incrementAndGet();
    }
  }

  public static void main(String[] args) throws Exception {
    Thread[] threads = new Thread[THREADS];
    for (int i = 0; i < THREADS; i++) {
      final int id = i;
      threads[i] = Thread.ofVirtual().start(() -> body(id));
    }
    for (Thread t : threads) t.join();
    check(failures.get() == 0, failures.get() + " virtual threads saw damaged data");

    // The scope still works for ordinary pool workers.
    ForkJoinPool pool = new ForkJoinPool(2);
    for (int i = 0; i < 200; i++) {
      pool.submit(() -> {
        if (WB.epoxyScratchDepth() > 0) scopedTasks.incrementAndGet();
        garbage();
      }).get();
    }
    pool.shutdown();
    check(scopedTasks.get() == 200, "only " + scopedTasks.get() + " of 200 pool tasks ran in a region");
    System.out.println("ok");
  }
}
