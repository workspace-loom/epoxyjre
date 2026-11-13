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

import java.util.ArrayList;
import java.util.HashMap;
import java.util.IdentityHashMap;

/**
 * @test TestScopedEvacuation
 * @requires vm.gc.Epoxy & vm.debug
 * @summary Values returned or thrown out of a scoped method are moved into the caller's memory intact
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:+EpoxyVerifyScratchExit
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-evacuation.txt gc.epoxy.TestScopedEvacuation
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:+EpoxyVerifyScratchExit
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-evacuation.txt -Xint gc.epoxy.TestScopedEvacuation
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:+EpoxyVerifyScratchExit
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-evacuation.txt -Xcomp gc.epoxy.TestScopedEvacuation
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:+EpoxyVerifyScratchExit
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-evacuation.txt -XX:TieredStopAtLevel=1 -Xcomp gc.epoxy.TestScopedEvacuation
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:+EpoxyVerifyScratchExit
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-evacuation.txt -XX:-TieredCompilation -Xcomp gc.epoxy.TestScopedEvacuation
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -DsmallCalls=20000
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-evacuation.txt gc.epoxy.TestScopedEvacuation
 */

public class TestScopedEvacuation extends ScratchTestSupport {

  static Graph kept;

  // ---- data classes -------------------------------------------------------------------------

  static class Ring {
    int id;
    Ring next;
    Ring prev;
  }

  static class Cell {
    int value;
  }

  static class SelfRef {
    SelfRef self;
  }

  // Does not override hashCode or equals: identity semantics only.
  static class Plain {
    int v;
  }

  static class IdentityHolder {
    Plain obj;
    int hash;
    IdentityHashMap<Plain, String> idMap;
    HashMap<Plain, String> hashMap;
  }

  static class BigArrays {
    long[] longs;
    Object[] boxed;
  }

  static class GraphPair {
    Graph a;
    Graph b;
  }

  static class RichException extends RuntimeException {
    final Graph graph;
    RichException(String message, Graph graph, Throwable cause) {
      super(message, cause);
      this.graph = graph;
    }
  }

  static class Small {
    int v;
  }

  // ---- non-scoped helpers (run inside the region of whichever scoped method calls them) ----

  static Ring buildRing(int n) {
    Ring[] r = new Ring[n];
    for (int i = 0; i < n; i++) {
      r[i] = new Ring();
      r[i].id = i;
    }
    for (int i = 0; i < n; i++) {
      r[i].next = r[(i + 1) % n];
      r[i].prev = r[(i + n - 1) % n];
    }
    return r[0];
  }

  static void churnThreeTimes() {
    churn(64);
    churn(64);
    churn(64);
  }

  // ---- verification -------------------------------------------------------------------------

  static void verifyRing(Ring x, int n, String what) {
    if (x == null) throw fail(what, "ring is missing");
    Ring cur = x;
    for (int i = 0; i < n; i++) {
      if (cur.id != i) throw fail(what, "ring element at step " + i + " has id " + cur.id);
      if (cur.next == null || cur.next.prev != cur) throw fail(what, "next.prev of element " + i + " is not the element");
      if (cur.prev == null || cur.prev.next != cur) throw fail(what, "prev.next of element " + i + " is not the element");
      cur = cur.next;
    }
    if (cur != x) throw fail(what, "walking next " + n + " times did not return to the start");
  }

  static void verifyShared(Object[] a, String what) {
    check(a != null && a.length == 2, what + ": array has the wrong shape");
    check(a[0] == a[1], what + ": the two elements are not the same object");
    check(a[0] instanceof Cell, what + ": element is not a Cell");
    check(((Cell) a[0]).value == 7, what + ": cell value is wrong before the write");
  }

  static void verifyBigArrays(BigArrays b, String what) {
    check(b != null && b.longs != null && b.boxed != null, what + ": arrays are missing");
    check(b.longs.length == 2_000_000, what + ": long array has the wrong length");
    check(b.boxed.length == 300_000, what + ": object array has the wrong length");
    for (int i = 0; i < b.longs.length; i++) {
      if (b.longs[i] != i * 7919L + 3) throw fail(what, "longs[" + i + "] is " + b.longs[i]);
    }
    for (int i = 0; i < b.boxed.length; i++) {
      Object o = b.boxed[i];
      if (!(o instanceof Integer) || ((Integer) o).intValue() != i + 1000) {
        throw fail(what, "boxed[" + i + "] is " + o);
      }
    }
  }

  static void verifyIdentity(IdentityHolder h, String what) {
    check(h != null && h.obj != null, what + ": holder is missing");
    check(System.identityHashCode(h.obj) == h.hash, what + ": identity hash code changed");
    check(h.idMap != null && h.idMap.size() == 1, what + ": identity map has the wrong size");
    check(h.hashMap != null && h.hashMap.size() == 1, what + ": hash map has the wrong size");
    check(h.idMap.get(h.obj) != null, what + ": identity map does not find the object");
    check(h.hashMap.get(h.obj) != null, what + ": hash map does not find the object");
  }

  static void verifyStrings(ArrayList<Object> list, String what) {
    check(list != null && list.size() == 5000, what + ": list has the wrong size");
    for (int i = 0; i < list.size(); i++) {
      Object expected = (i % 2 == 0) ? (Object) ("s-" + i) : (Object) Long.valueOf(i + 100_000L);
      if (!expected.equals(list.get(i))) throw fail(what, "element " + i + " is " + list.get(i));
    }
  }

  // ---- the scoped methods -------------------------------------------------------------------

  // a: a cyclic ring of 1,000 objects; return one element.
  static Ring ringScenario() {
    return buildRing(1000);
  }

  // b: one object referenced twice from the returned array.
  static Object[] sharedScenario() {
    Cell c = new Cell();
    c.value = 7;
    Object[] a = new Object[2];
    a[0] = c;
    a[1] = c;
    return a;
  }

  // c: an object whose field refers to itself.
  static SelfRef selfReferenceScenario() {
    SelfRef o = new SelfRef();
    o.self = o;
    return o;
  }

  // d: identity hash codes and identity-keyed and hash-keyed maps.
  static IdentityHolder identityScenario() {
    Plain p = new Plain();
    p.v = 9;
    IdentityHolder h = new IdentityHolder();
    h.obj = p;
    h.hash = System.identityHashCode(p);
    h.idMap = new IdentityHashMap<>();
    h.idMap.put(p, "p");
    h.hashMap = new HashMap<>();
    h.hashMap.put(p, "p");
    return h;
  }

  // e: large arrays, primitive and boxed.
  static BigArrays bigArrayScenario() {
    BigArrays b = new BigArrays();
    b.longs = new long[2_000_000];
    for (int i = 0; i < b.longs.length; i++) b.longs[i] = i * 7919L + 3;
    b.boxed = new Object[300_000];
    for (int i = 0; i < b.boxed.length; i++) b.boxed[i] = Integer.valueOf(i + 1000);
    return b;
  }

  // f: one graph stays in a static field, one is returned.
  static GraphPair mixedLifetimesScenario() {
    kept = buildGraph(31);
    GraphPair p = new GraphPair();
    p.a = kept;
    p.b = buildGraph(32);
    return p;
  }

  // g: a result referring to an argument from the caller and to a new graph.
  static GraphPair argumentScenario(Graph arg) {
    GraphPair p = new GraphPair();
    p.a = arg;
    p.b = buildGraph(41);
    return p;
  }

  // h: an exception with a payload, a cause and a suppressed exception.
  static void throwScenario() {
    Graph g = buildGraph(50);
    RichException e = new RichException("rich-50", g, new IllegalArgumentException("cause-50"));
    e.addSuppressed(new IllegalStateException("suppressed-50"));
    throw e;
  }

  // i: a ring returned through two scoped levels.
  static Ring innerLevel() {
    return buildRing(500);
  }

  static Ring outerLevel() {
    Ring r = innerLevel();
    garbage(16, 500);
    verifyRing(r, 500, "ring returned through two levels, before return");
    return r;
  }

  // j: boxed and string values in a list.
  static ArrayList<Object> stringsScenario() {
    ArrayList<Object> list = new ArrayList<>(5000);
    for (int i = 0; i < 5000; i++) {
      if (i % 2 == 0) {
        list.add("s-" + i);
      } else {
        list.add(Long.valueOf(i + 100_000L));
      }
    }
    return list;
  }

  // k: many small returned objects.
  static Small smallValue(int v) {
    Small s = new Small();
    s.v = v;
    return s;
  }

  // l: returns null after allocating garbage.
  static Object nullAfterGarbage() {
    garbage(4, 500);
    return null;
  }

  // l: a primitive computed from an array allocated in the region.
  static long arraySum(int n) {
    long[] a = new long[n];
    for (int i = 0; i < n; i++) a[i] = i * 3L;
    long sum = 0;
    for (int i = 0; i < n; i++) sum += a[i];
    return sum;
  }

  static void churn(int mb) {
    garbage(mb, 2000);
  }

  // ---- main ---------------------------------------------------------------------------------

  public static void main(String[] args) throws Exception {
    depth(0, "start");

    // a
    Ring ring = ringScenario();
    churnThreeTimes();
    verifyRing(ring, 1000, "ring");

    // b
    Object[] shared = sharedScenario();
    churnThreeTimes();
    verifyShared(shared, "shared");
    ((Cell) shared[0]).value = 42;
    check(((Cell) shared[1]).value == 42, "shared: write through element 0 not seen through element 1");

    // c
    SelfRef self = selfReferenceScenario();
    churnThreeTimes();
    check(self != null && self.self == self, "self reference");

    // d
    IdentityHolder identity = identityScenario();
    churnThreeTimes();
    verifyIdentity(identity, "identity");

    // e
    BigArrays big = bigArrayScenario();
    churnThreeTimes();
    verifyBigArrays(big, "big arrays");

    // f
    GraphPair mixed = mixedLifetimesScenario();
    churnThreeTimes();
    check(mixed.a == kept, "mixed: returned reference to the static graph is not the static graph");
    verifyGraph(kept, 31, "mixed: static graph");
    verifyGraph(mixed.b, 32, "mixed: returned graph");

    // g
    Graph external = buildGraph(40);
    GraphPair fromArg = argumentScenario(external);
    churnThreeTimes();
    check(fromArg.a == external, "argument: returned reference is not the argument");
    verifyGraph(external, 40, "argument: caller's graph");
    verifyGraph(fromArg.b, 41, "argument: new graph");

    // h
    RichException thrown = null;
    try {
      throwScenario();
    } catch (RichException e) {
      thrown = e;
    }
    check(thrown != null, "exception: nothing was thrown");
    churnThreeTimes();
    check(thrown.getMessage().equals("rich-50"), "exception: message");
    verifyGraph(thrown.graph, 50, "exception: graph");
    check(thrown.getCause() != null && thrown.getCause().getMessage().equals("cause-50"), "exception: cause");
    check(thrown.getSuppressed().length == 1, "exception: suppressed count");
    check(thrown.getSuppressed()[0].getMessage().equals("suppressed-50"), "exception: suppressed message");
    StackTraceElement[] trace = thrown.getStackTrace();
    check(trace.length > 0 && trace[0].getMethodName().equals("throwScenario"), "exception: stack trace");

    // i
    Ring twoLevels = outerLevel();
    churnThreeTimes();
    verifyRing(twoLevels, 500, "ring through two levels");

    // j
    ArrayList<Object> strings = stringsScenario();
    churnThreeTimes();
    verifyStrings(strings, "strings");

    // k
    Small[] smalls = new Small[Integer.getInteger("smallCalls", 1000)];   // each call is a whole-heap walk under the verifier
    for (int i = 0; i < smalls.length; i++) {
      smalls[i] = smallValue(i);
    }
    churnThreeTimes();
    for (int i = 0; i < smalls.length; i++) {
      check(smalls[i] != null && smalls[i].v == i, "small value " + i);
    }

    // l
    for (int i = 0; i < 100; i++) {
      check(nullAfterGarbage() == null, "null result");
    }
    long sum = arraySum(1000);
    check(sum == 1498500L, "array sum is " + sum);

    // Re-verify a, e, f and j at the end.
    verifyRing(ring, 1000, "ring, at the end");
    verifyBigArrays(big, "big arrays, at the end");
    verifyGraph(kept, 31, "mixed: static graph, at the end");
    check(mixed.a == kept, "mixed: identity, at the end");
    verifyGraph(mixed.b, 32, "mixed: returned graph, at the end");
    verifyStrings(strings, "strings, at the end");

    depth(0, "end");
  }
}
