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
 * @test TestScratchVerifierCatchesMissingBarrier
 * @requires vm.gc.Epoxy & vm.debug
 * @summary With the store barrier switched off, an escaping object is not promoted and the exit verifier aborts the VM
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport gc.epoxy.TestScratchEscapeFields
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run driver gc.epoxy.TestScratchVerifierCatchesMissingBarrier
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestScratchVerifierCatchesMissingBarrier {

  static OutputAnalyzer run(String barrier) throws Exception {
    return ProcessTools.executeTestJava(
        "-Xbootclasspath/a:.", "-XX:+UnlockDiagnosticVMOptions", "-XX:+WhiteBoxAPI",
        "-XX:+UseEpoxy", "-Xint", "-Xmx256m", "-XX:+EpoxyVerifyScratchExit",
        "-XX:-CreateCoredumpOnCrash", barrier,
        "gc.epoxy.TestScratchEscapeFields");
  }

  public static void main(String[] args) throws Exception {
    // Control: the same program passes with the barrier on.
    run("-XX:+EpoxyStoreBarrier").shouldHaveExitValue(0);

    // Without the barrier nothing is promoted; the first region exit after an
    // escape must be caught, naming the unpromoted object.
    OutputAnalyzer out = run("-XX:-EpoxyStoreBarrier");
    out.shouldNotHaveExitValue(0);
    out.shouldContain("EpoxyVerifier");
    out.shouldContain("unpromoted object");
  }
}
