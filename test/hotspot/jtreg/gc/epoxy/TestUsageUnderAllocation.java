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
 * @test TestUsageUnderAllocation
 * @requires vm.gc.Epoxy
 * @summary Heap usage numbers read while other threads allocate are always consistent
 * @run main/othervm -Xmx1g -XX:+UseEpoxy -XX:-UseTLAB gc.epoxy.TestUsageUnderAllocation
 * @run main/othervm -Xmx1g -XX:+UseEpoxy -XX:-UseTLAB -XX:EpoxyPageSize=64k gc.epoxy.TestUsageUnderAllocation
 */

import java.lang.management.ManagementFactory;
import java.lang.management.MemoryMXBean;
import java.lang.management.MemoryUsage;
import java.util.concurrent.atomic.AtomicLong;

public class TestUsageUnderAllocation {

  static final int THREADS = 8;
  static final long BUDGET_BYTES = 600L * 1024 * 1024;   // well under -Xmx1g
  static final int CHUNK = 20_000;                       // bytes per allocation: a new page every few calls

  static final AtomicLong allocated = new AtomicLong();
  static volatile Object sink;
  static volatile boolean done;

  public static void main(String[] args) throws Exception {
    Thread[] threads = new Thread[THREADS];
    for (int i = 0; i < THREADS; i++) {
      threads[i] = new Thread(() -> {
        while (allocated.addAndGet(CHUNK) < BUDGET_BYTES) {
          sink = new byte[CHUNK];
        }
      });
      threads[i].start();
    }

    Runtime rt = Runtime.getRuntime();
    MemoryMXBean bean = ManagementFactory.getMemoryMXBean();
    long max = rt.maxMemory();
    long polls = 0;
    Thread waiter = new Thread(() -> {
      try {
        for (Thread t : threads) {
          t.join();
        }
      } catch (InterruptedException e) {
      }
      done = true;
    });
    waiter.start();

    while (!done) {
      long free = rt.freeMemory();
      if (free < 0 || free > max) {
        throw new IllegalStateException("freeMemory() = " + free + " is outside [0, " + max + "]");
      }
      long total = rt.totalMemory();
      if (total < 0 || total > max) {
        throw new IllegalStateException("totalMemory() = " + total + " is outside [0, " + max + "]");
      }
      // Throws IllegalArgumentException if the VM reports used > committed.
      MemoryUsage usage = bean.getHeapMemoryUsage();
      if (usage.getUsed() > usage.getCommitted() || usage.getCommitted() > usage.getMax()) {
        throw new IllegalStateException("inconsistent usage: " + usage);
      }
      polls++;
    }
    waiter.join();
    if (polls < 100) {
      throw new IllegalStateException("only " + polls + " polls ran; the test did not exercise the race");
    }
  }
}
