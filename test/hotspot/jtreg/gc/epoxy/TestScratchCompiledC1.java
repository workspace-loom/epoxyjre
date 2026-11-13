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
 * @test TestScratchCompiledC1
 * @requires vm.gc.Epoxy & vm.debug & vm.compiler1.enabled
 * @summary Every scratch-region test also passes when its code is compiled by C1
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 *        gc.epoxy.TestScratchRegionFrees gc.epoxy.TestScratchEscapeFields gc.epoxy.TestScratchEscapeArrays
 *        gc.epoxy.TestScratchEscapeAtomics gc.epoxy.TestScratchEscapeThreads gc.epoxy.TestScratchEscapeRuntime
 *        gc.epoxy.TestScratchNested gc.epoxy.TestScratchNoEscape gc.epoxy.TestScratchThreadDiesInRegion
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run driver/timeout=2400 gc.epoxy.TestScratchCompiledC1
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestScratchCompiledC1 {

  static final String[] TESTS = {
    "TestScratchRegionFrees", "TestScratchEscapeFields", "TestScratchEscapeArrays",
    "TestScratchEscapeAtomics", "TestScratchEscapeThreads", "TestScratchEscapeRuntime",
    "TestScratchNested", "TestScratchNoEscape", "TestScratchThreadDiesInRegion",
  };

  // Two ways of getting compiled code: everything compiled before its first
  // call, and the normal mix of interpreted and compiled code.
  static final String[][] MODES = {
    { "-XX:TieredStopAtLevel=1", "-Xcomp" },
    { "-XX:TieredStopAtLevel=1", "-XX:Tier3InvocationThreshold=20", "-XX:Tier3BackEdgeThreshold=200" },
  };

  public static void main(String[] args) throws Exception {
    for (String[] mode : MODES) {
      for (String test : TESTS) {
        String[] common = {
          "-Xbootclasspath/a:.", "-XX:+UnlockDiagnosticVMOptions", "-XX:+WhiteBoxAPI",
          "-XX:+UseEpoxy", "-Xmx256m", "-XX:+EpoxyVerifyScratchExit", "-XX:-CreateCoredumpOnCrash",
          "-Dtest.jdk=" + System.getProperty("test.jdk"),   // one test runs jcmd on itself
        };
        String[] cmd = new String[common.length + mode.length + 1];
        System.arraycopy(common, 0, cmd, 0, common.length);
        System.arraycopy(mode, 0, cmd, common.length, mode.length);
        cmd[cmd.length - 1] = "gc.epoxy." + test;
        OutputAnalyzer out = ProcessTools.executeTestJava(cmd);
        System.out.println(test + " " + String.join(" ", mode) + ": exit " + out.getExitValue());
        out.shouldHaveExitValue(0);
      }
    }
  }
}
