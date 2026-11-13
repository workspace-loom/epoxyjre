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

import java.util.Arrays;
import java.util.HashMap;
import java.util.concurrent.CountDownLatch;
import jdk.test.whitebox.WhiteBox;

/**
 * Shared helpers for the TestScratchEscape*, TestScratchNested and TestScratchNoEscape tests.
 *
 * Not a test itself. The rule for everything here: no local variable or expression-stack
 * value may refer to an object allocated inside a scratch region across epoxyExitScratch().
 * Code that runs inside a region is therefore always a method returning void, and surviving
 * objects are reached afterwards only through a static field or an older object.
 *
 * A scenario is run once outside any region (warm-up: loads classes, resolves constants,
 * links call sites), then again inside a region, followed by churn and verification.
 */
public class ScratchTestSupport {

  static final WhiteBox WB = WhiteBox.getWhiteBox();

  static final int LIST_SIZE = 2_000;
  static final int MAP_SIZE = 500;
  static final int BIG_SIZE = 200_000;

  // Non-zero patterns that the churn garbage is copied from (System.arraycopy is fast under -Xint).
  static final byte[] BYTES = new byte[64 * 1024];
  static final int[] INTS = new int[16 * 1024];
  static final long[] LONGS = new long[8 * 1024];
  static {
    Arrays.fill(BYTES, (byte) 0x5A);
    Arrays.fill(INTS, 0x5A5A5A5A);
    Arrays.fill(LONGS, 0x5A5A5A5A5A5A5A5AL);
  }

  // False while a scenario is warmed up outside any region: then nothing can be promoted.
  static boolean realRun = true;
  static boolean warmed;

  /** A step of a scenario; may throw checked exceptions. */
  interface Step { void run() throws Exception; }

  // ---------------------------------------------------------------- recognisable data

  static final class Node {
    final int id;
    final String name;
    final int[] data;
    Node next;

    Node(int id, int tag) {
      this.id = id;
      this.name = name(id, tag);
      this.data = new int[8];
      for (int j = 0; j < 8; j++) data[j] = id + tag;
    }

    String label() { return name; }
  }

  static final class Graph {
    Node head;                        // LIST_SIZE nodes
    HashMap<Integer, String> map;     // MAP_SIZE entries
    long[] big;                       // BIG_SIZE longs: spans several pages
  }

  static String name(int id, int tag) {
    return "node-".concat(Integer.toString(id)).concat("/").concat(Integer.toString(tag));
  }

  static String value(int id, int tag) {
    return "value-".concat(Integer.toString(id)).concat("/").concat(Integer.toString(tag));
  }

  static long bigValue(int j, int tag) { return j * 7919L + tag; }

  static Node buildNodes(int n, int tag) {
    Node head = null;
    for (int i = n - 1; i >= 0; i--) {
      Node x = new Node(i, tag);
      x.next = head;
      head = x;
    }
    return head;
  }

  static long[] buildBig(int tag) {
    long[] a = new long[BIG_SIZE];
    for (int j = 0; j < a.length; j++) a[j] = bigValue(j, tag);
    return a;
  }

  static Graph buildGraph(int tag) {
    Graph g = new Graph();
    g.head = buildNodes(LIST_SIZE, tag);
    g.map = new HashMap<>();
    for (int i = 0; i < MAP_SIZE; i++) g.map.put(i, value(i, tag));
    g.big = buildBig(tag);
    return g;
  }

  // ---------------------------------------------------------------- verification

  static IllegalStateException fail(String what, String msg) {
    return new IllegalStateException(what + ": " + msg);
  }

  /** Throws IllegalStateException with a constant message unless ok. Never build msg eagerly in hot code. */
  static void check(boolean ok, String msg) {
    if (!ok) throw new IllegalStateException(msg);
  }

  static void verifyNode(Node n, int id, int tag, String what) {
    if (n == null) throw fail(what, "node " + id + " is missing");
    if (n.id != id) throw fail(what, "node has id " + n.id + ", expected " + id);
    if (n.name == null || !n.name.equals(name(id, tag))) throw fail(what, "node " + id + " has name " + n.name);
    if (n.data == null || n.data.length != 8) throw fail(what, "node " + id + " has a wrong data array");
    for (int j = 0; j < 8; j++) {
      if (n.data[j] != id + tag) throw fail(what, "node " + id + " data[" + j + "] is " + n.data[j]);
    }
  }

  static void verifyNodes(Node head, int n, int tag, String what) {
    Node x = head;
    for (int i = 0; i < n; i++, x = x.next) verifyNode(x, i, tag, what);
    if (x != null) throw fail(what, "list is longer than " + n);
  }

  static void verifyBig(long[] a, int tag, String what) {
    if (a == null) throw fail(what, "big array is missing");
    if (a.length != BIG_SIZE) throw fail(what, "big array has length " + a.length);
    for (int j = 0; j < a.length; j++) {
      if (a[j] != bigValue(j, tag)) throw fail(what, "big[" + j + "] is " + a[j]);
    }
  }

  static void verifyGraph(Graph g, int tag, String what) {
    if (g == null) throw fail(what, "graph is missing");
    verifyNodes(g.head, LIST_SIZE, tag, what);
    if (g.map == null || g.map.size() != MAP_SIZE) throw fail(what, "map is missing or has the wrong size");
    for (int i = 0; i < MAP_SIZE; i++) {
      String v = g.map.get(i);
      if (v == null || !v.equals(value(i, tag))) throw fail(what, "map entry " + i + " is " + v);
    }
    verifyBig(g.big, tag, what);
  }

  /** The object escaped: it must report promoted after a real run, and must not after the warm-up. */
  static void promoted(Object o, String what) {
    if (WB.epoxyIsPromoted(o) != realRun) {
      throw fail(what, realRun ? "escaped object is not promoted"
                               : "object allocated outside any region reports promoted");
    }
  }

  static void notPromoted(Object o, String what) {
    if (WB.epoxyIsPromoted(o)) throw fail(what, "object is promoted but did not escape");
  }

  static void promotedNodes(Node head, int n, String what) {
    promoted(head, what);
    Node x = head;
    for (int i = 0; i < n / 2; i++) x = x.next;
    promoted(x, what);
    promoted(x.name, what);
    promoted(x.data, what);
    while (x.next != null) x = x.next;
    promoted(x, what);
  }

  static void promotedGraph(Graph g, String what) {
    promoted(g, what);
    promotedNodes(g.head, LIST_SIZE, what);
    promoted(g.map, what);
    promoted(g.map.get(MAP_SIZE - 1), what);
    promoted(g.big, what);
  }

  // ---------------------------------------------------------------- regions

  static void enter() { check(WB.epoxyEnterScratch(), "could not enter a scratch region"); }

  static void exit() { WB.epoxyExitScratch(); }

  static void depth(int expected, String what) {
    int d = WB.epoxyScratchDepth();
    if (d != expected) throw fail(what, "scratch depth is " + d + ", expected " + expected);
  }

  /** Runs body inside a new scratch region. */
  static void inRegion(Step body) throws Exception {
    enter();
    body.run();
    exit();
  }

  /** Allocates about mb MB of non-zero garbage plus a swarm of small arrays. Nothing refers to it afterwards. */
  static void garbage(int mb, int smalls) {
    for (int i = 0; i < smalls; i++) {
      int[] a = new int[8];
      byte[] b = new byte[24];
      long[] c = new long[2];
      System.arraycopy(INTS, 0, a, 0, a.length);
      System.arraycopy(BYTES, 0, b, 0, b.length);
      System.arraycopy(LONGS, 0, c, 0, c.length);
    }
    for (int i = 0; i < mb / 4; i++) {
      byte[] b = new byte[1 << 20];
      for (int o = 0; o < b.length; o += BYTES.length) System.arraycopy(BYTES, 0, b, o, BYTES.length);
      int[] a = new int[1 << 18];
      for (int o = 0; o < a.length; o += INTS.length) System.arraycopy(INTS, 0, a, o, INTS.length);
      long[] c = new long[1 << 18];
      for (int o = 0; o < c.length; o += LONGS.length) System.arraycopy(LONGS, 0, c, o, LONGS.length);
    }
  }

  /** Three fresh regions of 64 MB garbage each: reuses the freed pages, so wrongly freed data is overwritten. */
  static void churn() {
    int d = WB.epoxyScratchDepth();
    for (int round = 0; round < 3; round++) {
      enter();
      garbage(64, 40_000);
      exit();
    }
    depth(d, "churn");
  }

  /** Links the support code outside any region. Call once at the start of main(). */
  static void warmUp() {
    if (warmed) return;
    warmed = true;
    garbage(4, 1_000);
    Graph g = buildGraph(0);
    verifyGraph(g, 0, "warm-up");
    notPromoted(g, "warm-up");
  }

  /**
   * One escape scenario. prepare (may be null) creates the pre-region state and runs outside any
   * region; body runs inside a region and makes objects escape; verify checks the survivors through
   * their long-lived root. All three are run once as a warm-up outside any region first.
   */
  static void scenario(String name, Step prepare, Step body, Step verify) {
    try {
      warmUp();
      realRun = false;
      if (prepare != null) prepare.run();
      body.run();
      verify.run();

      realRun = true;
      if (prepare != null) prepare.run();
      depth(0, "before");
      enter();
      depth(1, "inside");
      body.run();
      exit();
      depth(0, "after");
      churn();
      verify.run();
    } catch (Exception e) {
      throw new IllegalStateException(name + ": " + e, e);
    }
    System.out.println(name + ": ok");
  }

  static void scenario(String name, Step body, Step verify) {
    scenario(name, null, body, verify);
  }

  // ---------------------------------------------------------------- threads

  static void awaitLatch(CountDownLatch l) {
    try {
      l.await();
    } catch (InterruptedException e) {
      throw new IllegalStateException(e);
    }
  }

  static void joinThread(Thread t) {
    try {
      t.join();
    } catch (InterruptedException e) {
      throw new IllegalStateException(e);
    }
  }
}
