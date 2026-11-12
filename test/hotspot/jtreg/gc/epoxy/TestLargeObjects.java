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
 * @test TestLargeObjects
 * @requires vm.gc.Epoxy
 * @summary Objects larger than one heap page are contiguous, intact, and unaffected by later allocation
 * @run main/othervm -Xmx256m -XX:+UseEpoxy gc.epoxy.TestLargeObjects
 * @run main/othervm -Xmx256m -XX:+UseEpoxy -XX:-UseTLAB gc.epoxy.TestLargeObjects
 */

public class TestLargeObjects {

  static volatile Object sink;

  static void check(byte[] bytes, long[] longs, Object[] refs) {
    for (int i = 0; i < bytes.length; i++) {
      if (bytes[i] != (byte) (i * 31 + 7)) {
        throw new IllegalStateException("byte[] corrupted at " + i);
      }
    }
    for (int i = 0; i < longs.length; i++) {
      if (longs[i] != 0x0102030405060708L * i) {
        throw new IllegalStateException("long[] corrupted at " + i);
      }
    }
    for (int i = 0; i < refs.length; i++) {
      if (!(refs[i] instanceof Integer v) || v != i) {
        throw new IllegalStateException("Object[] corrupted at " + i);
      }
    }
  }

  public static void main(String[] args) {
    byte[] bytes = new byte[10 * 1024 * 1024];
    long[] longs = new long[1_000_000];
    Object[] refs = new Object[500_000];
    if (bytes.length != 10 * 1024 * 1024 || longs.length != 1_000_000 || refs.length != 500_000) {
      throw new IllegalStateException("wrong array length");
    }
    for (int i = 0; i < bytes.length; i++) {
      bytes[i] = (byte) (i * 31 + 7);
    }
    for (int i = 0; i < longs.length; i++) {
      longs[i] = 0x0102030405060708L * i;
    }
    for (int i = 0; i < refs.length; i++) {
      refs[i] = Integer.valueOf(i);
    }
    check(bytes, longs, refs);

    for (int i = 0; i < 100_000; i++) {
      sink = new int[1 + (i % 64)];
    }
    check(bytes, longs, refs);
  }
}
