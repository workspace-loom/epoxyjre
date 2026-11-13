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
 * @test TestScratchEscapeArrays
 * @requires vm.gc.Epoxy & vm.debug
 * @summary Objects stored into older arrays and collections survive the exit of a scratch region
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit gc.epoxy.TestScratchEscapeArrays
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:-UseTLAB gc.epoxy.TestScratchEscapeArrays
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:EpoxyPageSize=64k gc.epoxy.TestScratchEscapeArrays
 */

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

import static gc.epoxy.ScratchTestSupport.*;

public class TestScratchEscapeArrays {

  static final int N = 50;

  static Object[] pre;                  // allocated before the region
  static Object[] copy;
  static Object[] rangeCopy;
  static Node[] nodeCopy;
  static Object[][] grid;
  static ArrayList<Object> list;
  static HashMap<Integer, Node> map;
  static ArrayDeque<Node> deque;

  static void clear() {
    pre = null; copy = null; rangeCopy = null; nodeCopy = null; grid = null;
    list = null; map = null; deque = null;
  }

  // 1. aastore into an Object[] allocated before the region.
  static void storePrepare() { clear(); pre = new Object[4]; }
  static void storeBody() {
    pre[0] = buildGraph(1);
    pre[1] = buildNodes(200, 2);
    pre[2] = buildBig(3);
    pre[3] = new Node(7, 4);
  }
  static void storeVerify() {
    String w = "aastore into pre-region array";
    verifyGraph((Graph) pre[0], 1, w);
    promotedGraph((Graph) pre[0], w);
    verifyNodes((Node) pre[1], 200, 2, w);
    promotedNodes((Node) pre[1], 200, w);
    verifyBig((long[]) pre[2], 3, w);
    promoted(pre[2], w);
    verifyNode((Node) pre[3], 7, 4, w);
    promoted(pre[3], w);
    notPromoted(pre, w);
  }

  // 2. System.arraycopy of a whole in-region array into a pre-region array.
  static void copyAllPrepare() { clear(); pre = new Object[N]; }
  static void copyAllBody() {
    Object[] src = new Object[N];
    for (int i = 0; i < N - 1; i++) src[i] = new Node(i, 5);
    src[N - 1] = buildBig(5);
    System.arraycopy(src, 0, pre, 0, N);
  }
  static void copyAllVerify() {
    String w = "arraycopy of a whole array";
    if (pre.length != N) throw fail(w, "length changed");
    for (int i = 0; i < N - 1; i++) {
      verifyNode((Node) pre[i], i, 5, w);
      promoted(pre[i], w);
    }
    verifyBig((long[]) pre[N - 1], 5, w);
    promoted(pre[N - 1], w);
  }

  // 3. System.arraycopy of a sub-range with offsets: src[10..29] -> pre[30..49].
  static void copyRangePrepare() { clear(); pre = new Object[100]; }
  static void copyRangeBody() {
    Object[] src = new Object[60];
    for (int i = 0; i < src.length; i++) src[i] = new Node(i, 6);
    System.arraycopy(src, 10, pre, 30, 20);
  }
  static void copyRangeVerify() {
    String w = "arraycopy of a sub-range";
    for (int i = 0; i < pre.length; i++) {
      if (i >= 30 && i < 50) {
        verifyNode((Node) pre[i], i - 20, 6, w);
        promoted(pre[i], w);
      } else if (pre[i] != null) {
        throw fail(w, "slot " + i + " outside the range was written");
      }
    }
  }

  // 4. Arrays.copyOf / copyOfRange results stored in statics.
  static void copyOfPrepare() { clear(); }
  static void copyOfBody() {
    Object[] a = new Object[20];
    for (int i = 0; i < a.length; i++) a[i] = new Node(i, 7);
    copy = Arrays.copyOf(a, 40);
    rangeCopy = Arrays.copyOfRange(a, 5, 25);
    Node[] n = new Node[20];
    for (int i = 0; i < n.length; i++) n[i] = new Node(i, 8);
    nodeCopy = Arrays.copyOf(n, 30);
  }
  static void copyOfVerify() {
    String w = "Arrays.copyOf";
    if (copy.length != 40) throw fail(w, "copy has length " + copy.length);
    for (int i = 0; i < 40; i++) {
      if (i < 20) verifyNode((Node) copy[i], i, 7, w);
      else if (copy[i] != null) throw fail(w, "padding slot " + i + " is not null");
    }
    promoted(copy, w);
    if (rangeCopy.length != 20) throw fail(w, "range copy has length " + rangeCopy.length);
    for (int i = 0; i < 20; i++) {
      if (i < 15) verifyNode((Node) rangeCopy[i], i + 5, 7, w);
      else if (rangeCopy[i] != null) throw fail(w, "range padding slot " + i + " is not null");
    }
    if (nodeCopy.length != 30) throw fail(w, "node copy has length " + nodeCopy.length);
    for (int i = 0; i < 20; i++) {
      verifyNode(nodeCopy[i], i, 8, w);
      promoted(nodeCopy[i], w);
    }
    promoted(nodeCopy, w);
  }

  // 5. A pre-region ArrayList that grows (internal arrays are reallocated inside the region).
  static void listPrepare() { clear(); list = new ArrayList<>(); }
  static void listBody() {
    for (int i = 0; i < 1000; i++) list.add(new Node(i, 9));
    ArrayList<Object> more = new ArrayList<>();
    for (int i = 1000; i < 1100; i++) more.add(new Node(i, 9));
    list.addAll(more);
    list.add(buildBig(9));
  }
  static void listVerify() {
    String w = "ArrayList";
    if (list.size() != 1101) throw fail(w, "size is " + list.size());
    for (int i = 0; i < 1100; i++) {
      verifyNode((Node) list.get(i), i, 9, w);
      promoted(list.get(i), w);
    }
    verifyBig((long[]) list.get(1100), 9, w);
    notPromoted(list, w);
  }

  // 6. A pre-region HashMap that resizes (table arrays and entries are allocated inside the region).
  static void mapPrepare() { clear(); map = new HashMap<>(); }
  static void mapBody() {
    for (int i = 0; i < 1000; i++) map.put(i, new Node(i, 10));
  }
  static void mapVerify() {
    String w = "HashMap";
    if (map.size() != 1000) throw fail(w, "size is " + map.size());
    for (int i = 0; i < 1000; i++) {
      verifyNode(map.get(i), i, 10, w);
      promoted(map.get(i), w);
    }
    for (Map.Entry<Integer, Node> e : map.entrySet()) {
      if (e.getKey().intValue() != e.getValue().id) throw fail(w, "key " + e.getKey() + " maps to a wrong node");
    }
    notPromoted(map, w);
  }

  // 7. A pre-region ArrayDeque grown from both ends.
  static void dequePrepare() { clear(); deque = new ArrayDeque<>(); }
  static void dequeBody() {
    for (int i = 0; i < 500; i++) {
      if ((i & 1) == 0) deque.addLast(new Node(i, 11));
      else deque.addFirst(new Node(i, 11));
    }
  }
  static void dequeVerify() {
    String w = "ArrayDeque";
    if (deque.size() != 500) throw fail(w, "size is " + deque.size());
    int idx = 0;                          // odd ids descending, then even ids ascending
    for (Node n : deque) {
      verifyNode(n, idx < 250 ? 499 - 2 * idx : 2 * (idx - 250), 11, w);
      promoted(n, w);
      idx++;
    }
    notPromoted(deque, w);
  }

  // 8. new Object[10][10] built in the region and stored in a static.
  static void gridPrepare() { clear(); }
  static void gridBody() {
    Object[][] g = new Object[10][10];
    for (int i = 0; i < 10; i++) {
      for (int j = 0; j < 10; j++) g[i][j] = new Node(i * 10 + j, 12);
    }
    grid = g;
  }
  static void gridVerify() { verifyGrid(12, "Object[10][10]"); }

  // 8b. The same, but the array is stored first and filled after it has been promoted.
  static void gridLateBody() {
    grid = new Object[10][10];
    for (int i = 0; i < 10; i++) {
      for (int j = 0; j < 10; j++) grid[i][j] = new Node(i * 10 + j, 13);
    }
  }
  static void gridLateVerify() { verifyGrid(13, "Object[10][10] filled after the store"); }

  static void verifyGrid(int tag, String w) {
    if (grid.length != 10) throw fail(w, "outer length is " + grid.length);
    promoted(grid, w);
    for (int i = 0; i < 10; i++) {
      if (grid[i].length != 10) throw fail(w, "row " + i + " has length " + grid[i].length);
      promoted(grid[i], w);
      for (int j = 0; j < 10; j++) {
        verifyNode((Node) grid[i][j], i * 10 + j, tag, w);
        promoted(grid[i][j], w);
      }
    }
  }

  public static void main(String[] args) {
    warmUp();
    scenario("aastore", TestScratchEscapeArrays::storePrepare,
             TestScratchEscapeArrays::storeBody, TestScratchEscapeArrays::storeVerify);
    scenario("arraycopy whole", TestScratchEscapeArrays::copyAllPrepare,
             TestScratchEscapeArrays::copyAllBody, TestScratchEscapeArrays::copyAllVerify);
    scenario("arraycopy sub-range", TestScratchEscapeArrays::copyRangePrepare,
             TestScratchEscapeArrays::copyRangeBody, TestScratchEscapeArrays::copyRangeVerify);
    scenario("Arrays.copyOf", TestScratchEscapeArrays::copyOfPrepare,
             TestScratchEscapeArrays::copyOfBody, TestScratchEscapeArrays::copyOfVerify);
    scenario("ArrayList", TestScratchEscapeArrays::listPrepare,
             TestScratchEscapeArrays::listBody, TestScratchEscapeArrays::listVerify);
    scenario("HashMap", TestScratchEscapeArrays::mapPrepare,
             TestScratchEscapeArrays::mapBody, TestScratchEscapeArrays::mapVerify);
    scenario("ArrayDeque", TestScratchEscapeArrays::dequePrepare,
             TestScratchEscapeArrays::dequeBody, TestScratchEscapeArrays::dequeVerify);
    scenario("multi-dimensional array", TestScratchEscapeArrays::gridPrepare,
             TestScratchEscapeArrays::gridBody, TestScratchEscapeArrays::gridVerify);
    scenario("multi-dimensional array filled late", TestScratchEscapeArrays::gridPrepare,
             TestScratchEscapeArrays::gridLateBody, TestScratchEscapeArrays::gridLateVerify);
  }
}
