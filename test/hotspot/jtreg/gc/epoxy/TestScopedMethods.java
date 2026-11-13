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
 * @test TestScopedMethods
 * @requires vm.gc.Epoxy & vm.debug
 * @summary Methods named in the scope file run in their own scratch region; what they return, throw or store survives
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:+EpoxyVerifyScratchExit
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-methods.txt gc.epoxy.TestScopedMethods
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:+EpoxyVerifyScratchExit
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-methods.txt -Xint gc.epoxy.TestScopedMethods
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:+EpoxyVerifyScratchExit
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-methods.txt -Xcomp gc.epoxy.TestScopedMethods
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:+EpoxyVerifyScratchExit
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-methods.txt -XX:TieredStopAtLevel=1 -Xcomp gc.epoxy.TestScopedMethods
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:+EpoxyVerifyScratchExit
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-methods.txt -XX:-TieredCompilation -Xcomp gc.epoxy.TestScopedMethods
 */

public class TestScopedMethods extends ScratchTestSupport {

  static Graph staticGraph;

  // ---- the scoped methods -------------------------------------------------------------------

  static void garbage(int mb) {
    depth(1, "inside garbage");
    garbage(mb, 2000);
  }

  static Graph makeGraph(int tag) {
    return buildGraph(tag);                 // the returned graph leaves through the stack
  }

  static String makeString(int i) {
    return "scoped-" + i + "-" + (i * 31);
  }

  static Graph returnsNull(int mb) {
    garbage(mb, 500);
    return null;
  }

  static int primitive(int n) {
    int[] a = new int[1000];
    for (int i = 0; i < a.length; i++) a[i] = i + n;
    return a[999];
  }

  static void thrower(int tag) {
    Graph g = buildGraph(tag);
    throw new ScopedException("thrown-" + tag, g);
  }

  static void escaper(int tag) {
    staticGraph = buildGraph(tag);
  }

  static Graph outer(int tag) {
    depth(1, "inside outer");
    Graph g = buildGraph(tag);               // allocated in outer's region
    Graph fromInner = inner(tag + 1);        // returned out of inner's region into outer's frame
    verifyGraph(fromInner, tag + 1, "graph returned by inner, seen in outer");
    garbage(8, 500);                          // outer keeps allocating after inner is gone
    verifyGraph(fromInner, tag + 1, "graph returned by inner, after more allocation in outer");
    verifyGraph(g, tag, "outer's own graph");
    return fromInner;
  }

  static Graph inner(int tag) {
    depth(2, "inside inner");
    return buildGraph(tag);
  }

  static int recurse(int n) {
    depth(DEPTH_BASE - n + 1, "recursion");
    int[] local = new int[64];
    local[0] = n;
    return n == 0 ? 0 : recurse(n - 1) + local[0];
  }
  static final int DEPTH_BASE = 20;

  static int overloaded(int n) {              // listed: opens a region
    return WB.epoxyScratchDepth() * 100 + n;
  }

  static int overloaded(long n) {             // not listed
    return WB.epoxyScratchDepth() * 100 + (int) n;
  }

  static int notScoped() {
    return WB.epoxyScratchDepth();
  }

  static class ScopedException extends RuntimeException {
    final Graph graph;
    ScopedException(String message, Graph graph) {
      super(message);
      this.graph = graph;
    }
  }

  static class Worker implements Runnable {
    volatile Graph result;
    volatile int depthSeen = -1;
    public void run() {                       // listed: a whole thread body in one region
      depthSeen = WB.epoxyScratchDepth();
      result = buildGraph(77);
      ScratchTestSupport.garbage(8, 500);
    }
  }

  // ---- the checks ---------------------------------------------------------------------------

  static void churnOutside() {
    for (int i = 0; i < 3; i++) {
      garbage(64);                            // scoped: reuses whatever was freed
    }
    depth(0, "after churn");
  }

  public static void main(String[] args) throws Exception {
    depth(0, "start");
    check(notScoped() == 0, "an unlisted method opened a region");

    // 300 calls of 20 MB each is 6 GB through a 256 MB heap.
    for (int i = 0; i < 300; i++) {
      garbage(20);
    }
    depth(0, "after garbage");

    // A returned object graph.
    Graph g = makeGraph(1);
    depth(0, "after makeGraph");
    churnOutside();
    verifyGraph(g, 1, "returned graph");
    // It was moved into the caller's space, not promoted in place: no page of the
    // region is kept for it.
    notPromoted(g, "returned graph");

    // Many small returned objects.
    String[] strings = new String[2000];
    for (int i = 0; i < strings.length; i++) {
      strings[i] = makeString(i);
    }
    churnOutside();
    for (int i = 0; i < strings.length; i++) {
      check(strings[i].equals("scoped-" + i + "-" + (i * 31)), "returned string " + i);
    }

    // A scoped method that returns null, after other calls have returned objects.
    for (int i = 0; i < 50; i++) {
      check(returnsNull(4) == null, "null result " + i);
    }

    // A primitive result needs nothing and promotes nothing.
    check(primitive(5) == 1004, "primitive result");

    // An exception thrown out of a scoped method, with a payload and a stack trace.
    ScopedException caught = null;
    try {
      thrower(2);
    } catch (ScopedException e) {
      caught = e;
    }
    depth(0, "after thrower");
    churnOutside();
    check(caught != null && caught.getMessage().equals("thrown-2"), "exception message");
    verifyGraph(caught.graph, 2, "exception payload");
    StackTraceElement[] trace = caught.getStackTrace();
    check(trace.length >= 2 && trace[0].getMethodName().equals("thrower"), "exception stack trace");

    // An exception the VM creates, thrown from a scoped method.
    ArithmeticException vmException = null;
    try {
      recurseAndDivide();
    } catch (ArithmeticException e) {
      vmException = e;
    }
    churnOutside();
    check(vmException != null && vmException.getStackTrace().length > 0, "VM exception stack trace");

    // A store to a static.
    escaper(3);
    churnOutside();
    verifyGraph(staticGraph, 3, "graph stored in a static");

    // Nested scoped methods.
    Graph nested = outer(4);
    depth(0, "after outer");
    churnOutside();
    verifyGraph(nested, 5, "graph returned through two scoped methods");

    // Recursion: one region per activation.
    check(recurse(DEPTH_BASE) == DEPTH_BASE * (DEPTH_BASE + 1) / 2, "recursion result");
    depth(0, "after recursion");

    // Only the listed overload opens a region.
    check(overloaded(7) == 107, "listed overload: " + overloaded(7));
    check(overloaded(7L) == 7, "unlisted overload: " + overloaded(7L));

    // A scoped method that is a thread's whole body, on 20 threads in turn.
    for (int i = 0; i < 20; i++) {
      Worker w = new Worker();
      Thread t = new Thread(w);
      t.start();
      t.join();
      check(w.depthSeen == 1, "worker depth " + w.depthSeen);
      churnOutside();
      verifyGraph(w.result, 77, "graph stored by a scoped thread body");
    }

    // Everything checked so far is still intact at the end.
    verifyGraph(g, 1, "returned graph, at the end");
    verifyGraph(caught.graph, 2, "exception payload, at the end");
    verifyGraph(staticGraph, 3, "static graph, at the end");
    verifyGraph(nested, 5, "nested graph, at the end");
    depth(0, "end");
  }

  // thrower-like, but the exception comes from the VM: listed through recurse's region
  // being open when the division fails two calls down.
  static void recurseAndDivide() {
    divideInScope(0);
  }

  static int divideInScope(int zero) {
    return primitiveDivide(10, zero);
  }

  static int primitiveDivide(int a, int b) {
    return primitive(a) / b;
  }
}
