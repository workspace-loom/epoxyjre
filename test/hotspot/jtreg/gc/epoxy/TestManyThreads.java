/*
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
 */

package gc.epoxy;

/**
 * @test TestManyThreads
 * @requires vm.gc.Epoxy
 * @summary Test allocations from many threads
 *
 * @run main/othervm -XX:-UseTLAB -Xmx256m -Xss512k
 *                   -XX:+UseEpoxy
 *                   gc.epoxy.TestManyThreads
 *
 * @run main/othervm -XX:-UseTLAB -Xmx256m -Xss512k
 *                   -Xint
 *                   -XX:+UseEpoxy
 *                   gc.epoxy.TestManyThreads
 *
 * @run main/othervm -XX:-UseTLAB -Xmx256m -Xss512k
 *                   -Xbatch -Xcomp -XX:TieredStopAtLevel=1
 *                   -XX:+UseEpoxy
 *                   gc.epoxy.TestManyThreads
 *
 * @run main/othervm -XX:-UseTLAB -Xmx256m -Xss512k
 *                   -Xbatch -Xcomp -XX:-TieredCompilation
 *                   -XX:+UseEpoxy
 *                   gc.epoxy.TestManyThreads
 *
 * @run main/othervm -XX:+UseTLAB -Xmx256m -Xss512k
 *                   -XX:+UseEpoxy
 *                   gc.epoxy.TestManyThreads
 *
 * @run main/othervm -XX:+UseTLAB -Xmx256m -Xss512k
 *                   -Xint
 *                   -XX:+UseEpoxy
 *                   gc.epoxy.TestManyThreads
 *
 * @run main/othervm -XX:+UseTLAB -Xmx256m -Xss512k
 *                   -Xbatch -Xcomp -XX:TieredStopAtLevel=1
 *                   -XX:+UseEpoxy
 *                   gc.epoxy.TestManyThreads
 *
 * @run main/othervm -XX:+UseTLAB -Xmx256m -Xss512k
 *                   -Xbatch -Xcomp -XX:-TieredCompilation
 *                   -XX:+UseEpoxy
 *                   gc.epoxy.TestManyThreads
 */

import java.util.concurrent.atomic.*;

public class TestManyThreads {

  static final int THREADS = Integer.getInteger("threads", 64);
  static final int PER_THREAD = Integer.getInteger("count", 20_000);

  static volatile Throwable failed;
  static final AtomicInteger verified = new AtomicInteger();

  // Each thread allocates PER_THREAD small arrays stamped with its own index and
  // checks every stamp at the end: an allocation handed to two threads, or lost,
  // shows up as a wrong stamp.
  public static void workload(int id) {
    try {
      int[][] mine = new int[PER_THREAD][];
      for (int i = 0; i < PER_THREAD; i++) {
        int[] a = new int[8];
        for (int j = 0; j < a.length; j++) {
          a[j] = id * 1_000_000 + i;
        }
        mine[i] = a;
      }
      for (int i = 0; i < PER_THREAD; i++) {
        int[] a = mine[i];
        if (a.length != 8) {
          throw new IllegalStateException("thread " + id + ": wrong length at " + i);
        }
        for (int j = 0; j < a.length; j++) {
          if (a[j] != id * 1_000_000 + i) {
            throw new IllegalStateException("thread " + id + ": wrong stamp at " + i);
          }
        }
      }
      verified.incrementAndGet();
    } catch (Throwable e) {
      failed = e;
    }
  }

  public static void main(String[] args) throws Throwable {
    Thread[] threads = new Thread[THREADS];
    for (int c = 0; c < THREADS; c++) {
      final int id = c;
      threads[c] = new Thread(() -> workload(id));
      threads[c].start();
    }
    for (Thread t : threads) {
      t.join();
    }
    if (failed != null) {
      throw failed;
    }
    if (verified.get() != THREADS) {
      throw new IllegalStateException("Only " + verified.get() + " of " + THREADS + " threads verified");
    }
  }

}
