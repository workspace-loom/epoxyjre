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
 * @test TestScratchNested
 * @requires vm.gc.Epoxy & vm.debug
 * @summary Promotion across nested scratch regions: an inner exit keeps what escaped into the outer region
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit gc.epoxy.TestScratchNested
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:-UseTLAB gc.epoxy.TestScratchNested
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:EpoxyPageSize=64k gc.epoxy.TestScratchNested
 */

import static gc.epoxy.ScratchTestSupport.*;

public class TestScratchNested {

  static class Holder {
    Graph graph;
    Holder child;
    Holder outer;                       // refers to an older object
    Node node;
  }

  static Graph staticGraph;

  // Each level is a void method; its locals refer only to objects of its own or an enclosing
  // region and are dead before that region is left.

  // ---- 1. depth 2: an inner object stored into an outer object --------------------------------
  static void innerIntoOuter() {
    Holder outer = new Holder();                        // allocated in the outer region
    Object[] array = new Object[3];
    depth(1, "outer");
    enter();
    depth(2, "inner");
    outer.graph = buildGraph(1);                        // inner -> outer object: must survive the inner exit
    outer.node = new Node(5, 2);
    array[1] = buildNodes(300, 3);
    exit();
    depth(1, "after inner exit");
    churn();                                            // reuses the inner pages with garbage
    verifyGraph(outer.graph, 1, "inner object in outer field");
    promotedGraph(outer.graph, "inner object in outer field");
    verifyNode(outer.node, 5, 2, "inner object in outer field");
    verifyNodes((Node) array[1], 300, 3, "inner object in outer array");
    promoted(array[1], "inner object in outer array");
    notPromoted(outer, "outer object");                 // it did not escape the outer region
    depth(1, "end of outer");
  }

  // ---- 2. depth 2: an outer object stored into an inner object --------------------------------
  static void outerIntoInner() {
    Holder outer = new Holder();
    outer.node = new Node(6, 4);
    enter();
    outerIntoInnerBody(outer);                          // no local may refer to an inner object at the exit
    exit();
    churn();
    verifyNode(outer.node, 6, 4, "outer object after the inner exit");
    notPromoted(outer, "outer object after the inner exit");
    notPromoted(outer.node, "outer node after the inner exit");
  }

  static void outerIntoInnerBody(Holder outer) {
    Holder inner = new Holder();
    inner.outer = outer;                                // old into young: needs nothing
    inner.node = outer.node;
    inner.graph = buildGraph(4);
    if (inner.outer.node.id != 6) throw fail("outer into inner", "outer object changed");
    notPromoted(inner, "inner object");
    notPromoted(outer, "outer object");
  }

  // ---- 3. depth 2: an inner object stored into a static ---------------------------------------
  static void innerIntoStatic() {
    enter();
    staticGraph = buildGraph(5);
    exit();
    churn();
    verifyGraph(staticGraph, 5, "inner object in a static, between the exits");
    promotedGraph(staticGraph, "inner object in a static");
  }

  // ---- 4. depth 3 -----------------------------------------------------------------------------
  static Graph deepStatic;

  static void level1() {
    Holder o = new Holder();                            // allocated at depth 1
    depth(1, "level 1");
    enter();
    level2(o);
    exit();
    depth(1, "level 1 after the exit of level 2");
    churn();
    verifyLevels(o);
    churn();
    verifyLevels(o);
    promoted(deepStatic, "level 3 static");
  }

  static void level2(Holder o) {
    depth(2, "level 2");
    Holder m = new Holder();                            // allocated at depth 2
    m.node = new Node(9, 6);
    o.child = m;                                        // m escapes level 2 into o's lifetime
    Holder late = new Holder();
    late.node = new Node(10, 7);
    enter();
    level3(m, late);
    exit();
    depth(2, "level 2 after the exit of level 3");
    o.outer = late;                                     // escapes only now; carries its level 3 graph
    churn();                                            // depth 3 garbage while m and late are live
    verifyNode(m.node, 9, 6, "level 2 node");
    verifyGraph(m.graph, 7, "level 3 graph in a level 2 object");
    verifyGraph(late.graph, 8, "level 3 graph in a late-escaping level 2 object");
  }

  static void level3(Holder m, Holder late) {
    depth(3, "level 3");
    m.graph = buildGraph(7);                            // into an already escaped level 2 object
    late.graph = buildGraph(8);                         // into a level 2 object that escapes later
    deepStatic = buildGraph(9);                         // survives all three exits
  }

  static void verifyLevels(Holder o) {
    String w = "depth 3";
    verifyNode(o.child.node, 9, 6, w);
    promoted(o.child, w);
    verifyGraph(o.child.graph, 7, w);
    promotedGraph(o.child.graph, w);
    verifyNode(o.outer.node, 10, 7, w);
    verifyGraph(o.outer.graph, 8, w);
    promotedGraph(o.outer.graph, w);
    verifyGraph(deepStatic, 9, w);
    promotedGraph(deepStatic, w);
  }

  static void run(String name, Runnable body) {
    depth(0, name);
    enter();
    body.run();
    exit();
    depth(0, name);
    churn();
    System.out.println(name + ": ok");
  }

  public static void main(String[] args) {
    warmUp();
    new Holder().graph = buildGraph(0);   // links the test's own classes outside any region
    realRun = true;

    run("inner object in an outer field", TestScratchNested::innerIntoOuter);
    run("outer object in an inner object", TestScratchNested::outerIntoInner);
    staticGraph = null;
    run("inner object in a static", TestScratchNested::innerIntoStatic);
    verifyGraph(staticGraph, 5, "depth 2 static after both exits");
    promotedGraph(staticGraph, "depth 2 static after both exits");

    run("depth 3", TestScratchNested::level1);
    verifyGraph(deepStatic, 9, "depth 3 static after all exits");
    promotedGraph(deepStatic, "depth 3 static after all exits");
    depth(0, "end");
  }
}
