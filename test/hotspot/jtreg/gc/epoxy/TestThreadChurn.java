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
 * @test TestThreadChurn
 * @requires vm.gc.Epoxy
 * @summary Thousands of short-lived threads that each allocate a little must not each cost a page
 * @run main/othervm -Xmx64m -Xss256k -XX:+UseEpoxy gc.epoxy.TestThreadChurn
 */

public class TestThreadChurn {

  static final int THREADS = 3000;   // at one 256K page per thread this would need 750M
  static volatile Object sink;
  static volatile Throwable failed;

  public static void main(String[] args) throws Throwable {
    for (int i = 0; i < THREADS; i++) {
      final int id = i;
      Thread t = new Thread(() -> {
        try {
          int[] a = new int[16];
          a[0] = id;
          sink = a;
          if (a[0] != id) {
            throw new IllegalStateException("corrupted");
          }
        } catch (Throwable e) {
          failed = e;
        }
      });
      t.start();
      t.join();
    }
    if (failed != null) {
      throw failed;
    }
  }
}
