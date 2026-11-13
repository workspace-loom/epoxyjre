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
 * @test TestScratchNoEscape
 * @requires vm.gc.Epoxy & vm.debug
 * @summary Objects that do not escape a scratch region are not promoted and their memory is reused
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit gc.epoxy.TestScratchNoEscape
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:-UseTLAB gc.epoxy.TestScratchNoEscape
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:EpoxyPageSize=64k gc.epoxy.TestScratchNoEscape
 */

import static gc.epoxy.ScratchTestSupport.*;

public class TestScratchNoEscape {

  static final int CHUNK_BYTES = 64 * 1024 - 32;
  static final int CHUNKS = 800;                  // about 50 MB per round
  static final int ROUNDS = 100;
  static final int KEEP_ROUNDS = 10;

  static class Chunk {
    final byte[] data = new byte[CHUNK_BYTES];
    final int index;
    Chunk next;

    Chunk(int index) {
      this.index = index;
      data[0] = (byte) index;
      data[data.length - 1] = (byte) ~index;
    }
  }

  static class Holder {
    Holder next;
    Node node;
    Chunk chunk;
    Object[] array;
  }

  static final Node PRE = new Node(1, 0);         // allocated outside any region
  static final Node[] KEEP = new Node[KEEP_ROUNDS];

  /**
   * Builds a graph of 'chunks' 64 KB chunks and 2,000 nodes that stays local to the region, links
   * region objects to each other (also in a cycle), and checks that nothing is promoted.
   */
  static void buildLocal(int chunks, int round) {
    String w = "non-escaping graph";
    Chunk first = null;
    Chunk prev = null;
    for (int i = 0; i < chunks; i++) {
      Chunk c = new Chunk(i);
      if (prev == null) first = c; else prev.next = c;       // region object stored into region object
      prev = c;
    }
    notPromoted(first, w);
    notPromoted(first.data, w);
    notPromoted(prev, w);
    Chunk mid = first;
    for (int i = 0; i < chunks / 2; i++) mid = mid.next;
    notPromoted(mid, w);

    Node list = buildNodes(LIST_SIZE, round);
    notPromoted(list, w);
    notPromoted(list.next.name, w);
    notPromoted(list.next.data, w);

    Holder a = new Holder();
    Holder b = new Holder();
    a.next = b;                                              // cycle through same-region stores
    b.next = a;
    a.node = list;
    b.chunk = first;
    a.array = new Object[] { b, list, mid };
    b.node = PRE;                                            // an older object stored into a region object
    notPromoted(a, w);
    notPromoted(b, w);
    notPromoted(a.array, w);
    notPromoted(list, w);
    notPromoted(first, w);
    notPromoted(mid, w);
    notPromoted(PRE, w);

    int count = 0;                                           // the graph is still intact
    for (Chunk c = first; c != null; c = c.next, count++) {
      if (c.index != count || c.data[0] != (byte) count || c.data[c.data.length - 1] != (byte) ~count) {
        throw fail(w, "chunk " + count + " is corrupted");
      }
    }
    if (count != chunks) throw fail(w, "chain has " + count + " chunks");
    verifyNodes(a.node, LIST_SIZE, round, w);
    if (a.next.next != a) throw fail(w, "cycle is broken");
  }

  // Warm-up outside any region: the same code on a small graph.
  static void warmUpLocal() {
    buildLocal(4, 0);
  }

  // The 100-round run: 5 GB through a 256 MB heap.
  static void freed() {
    for (int round = 0; round < ROUNDS; round++) {
      depth(0, "before round");
      enter();
      buildLocal(CHUNKS, round);
      exit();
      depth(0, "after round");
    }
    System.out.println("non-escaping graphs are freed: ok");
  }

  // Each round also lets one tiny node escape: it must survive while the rest is reused.
  static void keepOne(int chunks, int round) {
    buildLocal(chunks, round);
    KEEP[round] = new Node(round, 100);
  }

  static void mixed() throws Exception {
    for (int round = 0; round < KEEP_ROUNDS; round++) {
      final int r = round;
      inRegion(() -> keepOne(CHUNKS, r));
    }
    for (int round = 0; round < KEEP_ROUNDS; round++) {
      verifyNode(KEEP[round], round, 100, "tiny node that escaped");
      promoted(KEEP[round], "tiny node that escaped");
    }
    churn();
    for (int round = 0; round < KEEP_ROUNDS; round++) {
      verifyNode(KEEP[round], round, 100, "tiny node that escaped, after churn");
    }
    System.out.println("mixed escaping and non-escaping: ok");
  }

  public static void main(String[] args) throws Exception {
    warmUp();
    realRun = true;
    warmUpLocal();
    keepOne(4, 0);                                // links the code of mixed() outside any region
    KEEP[0] = null;
    freed();
    mixed();
  }
}
