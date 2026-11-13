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

/**
 * @test TestScopedSmallEscapes
 * @requires vm.gc.Epoxy & vm.debug
 * @summary A few small objects escaping from each of many regions must not keep the rest of the regions' memory
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm/timeout=1200 -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-small-escapes.txt gc.epoxy.TestScopedSmallEscapes
 * @run main/othervm/timeout=1200 -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:-TieredCompilation
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-small-escapes.txt gc.epoxy.TestScopedSmallEscapes
 * @run main/othervm/timeout=1200 -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:TieredStopAtLevel=1
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-small-escapes.txt gc.epoxy.TestScopedSmallEscapes
 * @run main/othervm/timeout=1200 -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xmx256m -XX:+EpoxyVerifyScratchExit -DsmallRun=true
 *                   -XX:EpoxyScopeFile=${test.src}/scoped-small-escapes.txt gc.epoxy.TestScopedSmallEscapes
 */

public class TestScopedSmallEscapes extends ScratchTestSupport {

  static final int N = Boolean.getBoolean("smallRun") ? 2_000 : 100_000;

  static class Item {
    int id;
    int[] data;
    Item next;
  }

  static Item[] items = new Item[N];
  static Item[] second = new Item[N];
  static Item[] chains = new Item[N / 5];
  static ArrayList<String> names = new ArrayList<>();
  static Item[] slots = new Item[64];
  static int sink;

  // ---- helpers (not scoped) -----------------------------------------------------------------

  /** About 20 KB of non-zero garbage when arrays is 200. Nothing refers to it afterwards. */
  static void smallGarbage(int arrays) {
    int sum = 0;
    for (int k = 0; k < arrays; k++) {
      int[] a = new int[20];
      for (int j = 0; j < a.length; j++) a[j] = k + j;
      for (int j = 0; j < a.length; j++) sum += a[j];
    }
    sink += sum;
  }

  static Item newItem(int id) {
    Item it = new Item();
    it.id = id;
    it.data = new int[4];
    for (int j = 0; j < it.data.length; j++) it.data[j] = id;
    return it;
  }

  static void verifyItem(Item it, int id, String what) {
    check(it != null, what + ": item " + id + " is missing");
    check(it.id == id, what + ": item " + id + " has id " + it.id);
    check(it.data != null && it.data.length == 4, what + ": item " + id + " has a wrong data array");
    for (int j = 0; j < 4; j++) {
      check(it.data[j] == id, what + ": item " + id + " data[" + j + "] is " + it.data[j]);
    }
  }

  // ---- the scoped methods -------------------------------------------------------------------

  static void oneSurvivor(int i) {
    smallGarbage(200);
    items[i] = newItem(i);
  }

  static void survivorInTheMiddle(int i) {
    smallGarbage(100);
    second[i] = newItem(i);
    smallGarbage(100);
  }

  static void chainSurvivors(int i) {
    Item head = null;
    Item tail = null;
    for (int k = 0; k < 5; k++) {
      smallGarbage(42);
      Item it = newItem(i * 5 + k);
      if (tail == null) {
        head = it;
      } else {
        tail.next = it;
      }
      tail = it;
    }
    chains[i] = head;
  }

  static void addName(int i) {
    smallGarbage(200);
    names.add("name-" + i);
  }

  static void replaceSlot(int i) {
    smallGarbage(200);
    slots[i % 64] = newItem(i);
  }

  // ---- the checks ---------------------------------------------------------------------------

  public static void main(String[] args) throws Exception {
    depth(0, "start");

    // A: one small survivor per call.
    for (int i = 0; i < N; i++) {
      oneSurvivor(i);
    }
    depth(0, "after A calls");
    for (int i = 0; i < N; i++) {
      verifyItem(items[i], i, "A");
    }
    System.out.println("A: ok");

    // B: survivor in the middle of the garbage.
    for (int i = 0; i < N; i++) {
      survivorInTheMiddle(i);
    }
    depth(0, "after B calls");
    for (int i = 0; i < N; i++) {
      verifyItem(second[i], i, "B");
    }
    System.out.println("B: ok");

    // C: a few linked survivors per call; only the head is stored.
    for (int i = 0; i < N / 5; i++) {
      chainSurvivors(i);
    }
    depth(0, "after C calls");
    for (int i = 0; i < N / 5; i++) {
      Item x = chains[i];
      check(x != null, "C: chain " + i + " is missing");
      for (int k = 0; k < 5; k++) {
        verifyItem(x, i * 5 + k, "C: chain " + i + " element " + k);
        x = x.next;
      }
      check(x == null, "C: chain " + i + " is longer than 5");
    }
    System.out.println("C: ok");

    // D: a growing collection; its internal array is reallocated inside scoped calls.
    for (int i = 0; i < N; i++) {
      addName(i);
    }
    depth(0, "after D calls");
    check(names.size() == N, "D: size is " + names.size());
    for (int i = 0; i < N; i++) {
      check(names.get(i).equals("name-" + i), "D: element " + i + " is " + names.get(i));
    }
    System.out.println("D: ok");

    // E: survivors that are later dropped; each of the 64 slots keeps its last Item.
    for (int i = 0; i < N; i++) {
      replaceSlot(i);
    }
    depth(0, "after E calls");
    for (int k = 0; k < 64; k++) {
      int last = k + 64 * ((N - 1 - k) / 64);
      verifyItem(slots[k], last, "E: slot " + k);
    }
    System.out.println("E: ok");

    // F: scenario A's array once more, after all the other scenarios.
    for (int i = 0; i < N; i++) {
      verifyItem(items[i], i, "F");
    }
    System.out.println("F: ok");

    depth(0, "end");
  }
}
