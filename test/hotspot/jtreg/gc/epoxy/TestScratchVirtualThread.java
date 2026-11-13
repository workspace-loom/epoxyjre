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
 * @test TestScratchVirtualThread
 * @requires vm.gc.Epoxy & vm.debug
 * @summary A virtual thread cannot enter a scratch region: regions belong to platform threads
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit gc.epoxy.TestScratchVirtualThread
 */

import java.util.concurrent.atomic.AtomicReference;

public class TestScratchVirtualThread extends ScratchTestSupport {

  public static void main(String[] args) throws Exception {
    AtomicReference<String> problem = new AtomicReference<>();
    Thread t = Thread.ofVirtual().start(() -> {
      if (WB.epoxyEnterScratch()) {
        problem.set("a virtual thread entered a scratch region");
        WB.epoxyExitScratch();
      } else if (WB.epoxyScratchDepth() != 0) {
        problem.set("depth changed after a refused enter");
      }
    });
    t.join();
    if (problem.get() != null) {
      throw new IllegalStateException(problem.get());
    }

    // A platform thread still can.
    enter();
    depth(1, "platform thread");
    exit();
  }
}
