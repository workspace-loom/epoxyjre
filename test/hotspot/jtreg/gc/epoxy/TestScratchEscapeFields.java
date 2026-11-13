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
 * @test TestScratchEscapeFields
 * @requires vm.gc.Epoxy & vm.debug
 * @summary Objects stored into static and instance fields survive the exit of a scratch region
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit gc.epoxy.TestScratchEscapeFields
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:-UseTLAB gc.epoxy.TestScratchEscapeFields
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:EpoxyPageSize=64k gc.epoxy.TestScratchEscapeFields
 */

import static gc.epoxy.ScratchTestSupport.*;

public class TestScratchEscapeFields {

  static class Holder {
    Graph graph;
    Holder inner;
    volatile Graph vgraph;
  }

  static Graph staticGraph;
  static volatile Graph volatileGraph;
  static Graph kept;
  static Holder holder;

  // 1. A static field.
  static void staticPrepare() { staticGraph = null; }
  static void staticBody() { staticGraph = buildGraph(1); }
  static void staticVerify() {
    verifyGraph(staticGraph, 1, "static field");
    promotedGraph(staticGraph, "static field");
  }

  // 1b. A volatile static field takes a different path through the interpreter.
  static void volatileStaticPrepare() { volatileGraph = null; }
  static void volatileStaticBody() { volatileGraph = buildGraph(2); }
  static void volatileStaticVerify() {
    verifyGraph(volatileGraph, 2, "volatile static field");
    promotedGraph(volatileGraph, "volatile static field");
  }

  // 2. A field of an object allocated before the region.
  static void instancePrepare() { holder = new Holder(); }
  static void instanceBody() {
    holder.graph = buildGraph(3);
    holder.vgraph = buildGraph(4);
  }
  static void instanceVerify() {
    verifyGraph(holder.graph, 3, "pre-region instance field");
    promotedGraph(holder.graph, "pre-region instance field");
    verifyGraph(holder.vgraph, 4, "pre-region volatile instance field");
    promotedGraph(holder.vgraph, "pre-region volatile instance field");
    notPromoted(holder, "pre-region holder");
  }

  // 3. A field of an object that was itself promoted earlier in the same region.
  static void afterPrepare() { holder = null; }
  static void afterBody() {
    Holder a = new Holder();
    holder = a;                          // A escapes to a static ...
    a.graph = buildGraph(5);             // ... then B is stored into A.field
    a.inner = new Holder();              // chain: a.inner is stored into a promoted object,
    a.inner.graph = buildGraph(6);       // and the graph into a.inner, which is promoted by now
  }
  static void afterVerify() {
    promoted(holder, "field of promoted object");
    verifyGraph(holder.graph, 5, "field of promoted object");
    promotedGraph(holder.graph, "field of promoted object");
    promoted(holder.inner, "field of promoted object");
    verifyGraph(holder.inner.graph, 6, "field of promoted chain");
    promotedGraph(holder.inner.graph, "field of promoted chain");
  }

  // 3b. The opposite order: the graphs hang off an object that escapes last.
  static void lateBody() {
    Holder a = new Holder();
    a.graph = buildGraph(7);
    a.inner = new Holder();
    a.inner.graph = buildGraph(8);
    holder = a;                          // everything reachable from a must now survive
  }
  static void lateVerify() {
    promoted(holder, "late escape");
    verifyGraph(holder.graph, 7, "late escape");
    promotedGraph(holder.graph, "late escape");
    verifyGraph(holder.inner.graph, 8, "late escape (inner)");
    promotedGraph(holder.inner.graph, "late escape (inner)");
  }

  // 4. Overwrite a static that already holds a promoted graph.
  static void firstBody() { staticGraph = buildGraph(9); }
  static void secondBody() {
    kept = staticGraph;                  // already promoted: a plain reference copy
    staticGraph = buildGraph(10);
  }

  static void overwrite() throws Exception {
    warmUp();
    realRun = false;
    firstBody();
    secondBody();
    kept = null;
    staticGraph = null;

    realRun = true;
    inRegion(TestScratchEscapeFields::firstBody);
    churn();
    verifyGraph(staticGraph, 9, "overwrite: first graph");
    promotedGraph(staticGraph, "overwrite: first graph");

    inRegion(TestScratchEscapeFields::secondBody);
    churn();
    verifyGraph(kept, 9, "overwrite: first graph, still referenced");
    verifyGraph(staticGraph, 10, "overwrite: second graph");
    promotedGraph(staticGraph, "overwrite: second graph");

    kept = null;                         // the first graph is dropped and no longer checked
    churn();
    verifyGraph(staticGraph, 10, "overwrite: second graph after the first was dropped");
    System.out.println("overwrite static: ok");
  }

  public static void main(String[] args) throws Exception {
    warmUp();
    scenario("static field", TestScratchEscapeFields::staticPrepare,
             TestScratchEscapeFields::staticBody, TestScratchEscapeFields::staticVerify);
    scenario("volatile static field", TestScratchEscapeFields::volatileStaticPrepare,
             TestScratchEscapeFields::volatileStaticBody, TestScratchEscapeFields::volatileStaticVerify);
    scenario("pre-region instance field", TestScratchEscapeFields::instancePrepare,
             TestScratchEscapeFields::instanceBody, TestScratchEscapeFields::instanceVerify);
    scenario("field of a promoted object", TestScratchEscapeFields::afterPrepare,
             TestScratchEscapeFields::afterBody, TestScratchEscapeFields::afterVerify);
    scenario("late escape", TestScratchEscapeFields::afterPrepare,
             TestScratchEscapeFields::lateBody, TestScratchEscapeFields::lateVerify);
    overwrite();
  }
}
