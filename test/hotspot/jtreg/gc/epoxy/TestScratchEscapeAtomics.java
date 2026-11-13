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
 * @test TestScratchEscapeAtomics
 * @requires vm.gc.Epoxy & vm.debug
 * @summary Atomic, VarHandle and reflective stores of region objects survive the exit of a scratch region
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit gc.epoxy.TestScratchEscapeAtomics
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:-UseTLAB gc.epoxy.TestScratchEscapeAtomics
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:EpoxyPageSize=64k gc.epoxy.TestScratchEscapeAtomics
 */

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicReferenceArray;

import static gc.epoxy.ScratchTestSupport.*;

public class TestScratchEscapeAtomics {

  static final int LEN = 100;           // length of the node lists handed over
  static final Node SENTINEL = new Node(-1, 0);   // allocated outside any region

  static AtomicReference<Node> ref, ref2, ref3;
  static AtomicReferenceArray<Node> refs;

  // 1. AtomicReference.set / lazySet / setRelease.
  static void setPrepare() {
    ref = new AtomicReference<>();
    ref2 = new AtomicReference<>();
    ref3 = new AtomicReference<>();
  }
  static void setBody() {
    ref.set(buildNodes(LEN, 1));
    ref2.lazySet(buildNodes(LEN, 2));
    ref3.setRelease(buildNodes(LEN, 3));
  }
  static void setVerify() {
    String w = "AtomicReference.set";
    verifyNodes(ref.get(), LEN, 1, w);
    verifyNodes(ref2.get(), LEN, 2, w);
    verifyNodes(ref3.get(), LEN, 3, w);
    promotedNodes(ref.get(), LEN, w);
    promotedNodes(ref2.get(), LEN, w);
    promotedNodes(ref3.get(), LEN, w);
    notPromoted(ref, w);
  }

  // 2. AtomicReference.compareAndSet / compareAndExchange; a failed CAS installs nothing.
  static void casPrepare() {
    ref = new AtomicReference<>(SENTINEL);
    ref2 = new AtomicReference<>(SENTINEL);
  }
  static void casBody() {
    check(ref.compareAndSet(SENTINEL, buildNodes(LEN, 4)), "compareAndSet failed");
    check(!ref.compareAndSet(SENTINEL, buildNodes(LEN, 99)), "compareAndSet succeeded on a stale value");
    check(ref2.compareAndExchange(SENTINEL, buildNodes(LEN, 5)) == SENTINEL, "compareAndExchange failed");
  }
  static void casVerify() {
    String w = "AtomicReference.compareAndSet";
    verifyNodes(ref.get(), LEN, 4, w);
    promotedNodes(ref.get(), LEN, w);
    verifyNodes(ref2.get(), LEN, 5, w);
    promotedNodes(ref2.get(), LEN, w);
  }

  // 3. AtomicReference.getAndSet / updateAndGet; the first value of ref is replaced by a second one.
  static void getAndSetPrepare() {
    ref = new AtomicReference<>(SENTINEL);
    ref2 = new AtomicReference<>(SENTINEL);
  }
  static void getAndSetBody() {
    check(ref.getAndSet(buildNodes(LEN, 6)) == SENTINEL, "getAndSet returned a wrong old value");
    ref.getAndSet(buildNodes(LEN, 7));
    ref2.updateAndGet(x -> buildNodes(LEN, 8));
  }
  static void getAndSetVerify() {
    String w = "AtomicReference.getAndSet";
    verifyNodes(ref.get(), LEN, 7, w);
    promotedNodes(ref.get(), LEN, w);
    verifyNodes(ref2.get(), LEN, 8, w);
    promotedNodes(ref2.get(), LEN, w);
  }

  // 4. AtomicReferenceArray.
  static void arrayPrepare() { refs = new AtomicReferenceArray<>(8); }
  static void arrayBody() {
    refs.set(0, buildNodes(LEN, 10));
    refs.lazySet(1, buildNodes(LEN, 11));
    check(refs.compareAndSet(2, null, buildNodes(LEN, 12)), "array compareAndSet failed");
    check(refs.getAndSet(3, buildNodes(LEN, 13)) == null, "array getAndSet returned a value");
    check(refs.compareAndExchange(4, null, buildNodes(LEN, 14)) == null, "array compareAndExchange failed");
    refs.updateAndGet(5, x -> buildNodes(LEN, 15));
  }
  static void arrayVerify() {
    String w = "AtomicReferenceArray";
    for (int i = 0; i < 6; i++) {
      verifyNodes(refs.get(i), LEN, 10 + i, w);
      promotedNodes(refs.get(i), LEN, w);
    }
    if (refs.get(6) != null || refs.get(7) != null) throw fail(w, "an untouched slot is not null");
  }

  // 5. VarHandles: instance field, static field and array element.
  static class Holder {
    Object f1, f2, f3;
    static Object sf;
  }
  static final VarHandle F1, F2, F3, SF, ARR;
  static {
    try {
      MethodHandles.Lookup l = MethodHandles.lookup();
      F1 = l.findVarHandle(Holder.class, "f1", Object.class);
      F2 = l.findVarHandle(Holder.class, "f2", Object.class);
      F3 = l.findVarHandle(Holder.class, "f3", Object.class);
      SF = l.findStaticVarHandle(Holder.class, "sf", Object.class);
      ARR = MethodHandles.arrayElementVarHandle(Object[].class);
    } catch (ReflectiveOperationException e) {
      throw new ExceptionInInitializerError(e);
    }
  }
  static Holder holder;
  static Object[] varr;

  static void vhPrepare() {
    holder = new Holder();
    holder.f2 = SENTINEL;
    holder.f3 = SENTINEL;
    Holder.sf = null;
    varr = new Object[4];
  }
  static void vhBody() {
    F1.set(holder, (Object) buildNodes(LEN, 20));
    check(F2.compareAndSet(holder, (Object) SENTINEL, (Object) buildNodes(LEN, 21)), "VarHandle compareAndSet failed");
    Object old = (Object) F3.getAndSet(holder, (Object) buildNodes(LEN, 22));
    check(old == SENTINEL, "VarHandle getAndSet returned a wrong old value");
    SF.set((Object) buildNodes(LEN, 23));
    ARR.set(varr, 0, (Object) buildNodes(LEN, 24));
    check(ARR.compareAndSet(varr, 1, (Object) null, (Object) buildNodes(LEN, 25)), "array VarHandle compareAndSet failed");
    Object prev = (Object) ARR.getAndSet(varr, 2, (Object) buildNodes(LEN, 26));
    check(prev == null, "array VarHandle getAndSet returned a value");
  }
  static void vhVerify() {
    String w = "VarHandle";
    verifyNodes((Node) holder.f1, LEN, 20, w);
    promotedNodes((Node) holder.f1, LEN, w);
    verifyNodes((Node) holder.f2, LEN, 21, w);
    promotedNodes((Node) holder.f2, LEN, w);
    verifyNodes((Node) holder.f3, LEN, 22, w);
    promotedNodes((Node) holder.f3, LEN, w);
    verifyNodes((Node) Holder.sf, LEN, 23, w);
    promotedNodes((Node) Holder.sf, LEN, w);
    for (int i = 0; i < 3; i++) {
      verifyNodes((Node) varr[i], LEN, 24 + i, w);
      promotedNodes((Node) varr[i], LEN, w);
    }
    if (varr[3] != null) throw fail(w, "an untouched slot is not null");
    notPromoted(holder, w);
  }

  // 6. java.lang.reflect.Field.set on a static and on instance fields.
  static class Target {
    Node inst;
    Object inst2;
    static Node stat;
  }
  static final Field F_INST, F_INST2, F_STAT;
  static {
    try {
      F_INST = Target.class.getDeclaredField("inst");
      F_INST2 = Target.class.getDeclaredField("inst2");
      F_STAT = Target.class.getDeclaredField("stat");
      F_INST.setAccessible(true);
      F_INST2.setAccessible(true);
      F_STAT.setAccessible(true);
    } catch (ReflectiveOperationException e) {
      throw new ExceptionInInitializerError(e);
    }
  }
  static Target target;

  static void fieldPrepare() {
    target = new Target();
    Target.stat = null;
  }
  static void fieldBody() throws Exception {
    F_STAT.set(null, buildNodes(LEN, 30));
    F_INST.set(target, buildNodes(LEN, 31));
    F_INST2.set(target, buildNodes(LEN, 32));
  }
  static void fieldVerify() {
    String w = "Field.set";
    verifyNodes(Target.stat, LEN, 30, w);
    promotedNodes(Target.stat, LEN, w);
    verifyNodes(target.inst, LEN, 31, w);
    promotedNodes(target.inst, LEN, w);
    verifyNodes((Node) target.inst2, LEN, 32, w);
    promotedNodes((Node) target.inst2, LEN, w);
  }

  // 7. java.lang.reflect.Array.set into Object[] and Node[] allocated before the region.
  static Object[] objArr;
  static Node[] nodeArr;

  static void arrSetPrepare() {
    objArr = new Object[3];
    nodeArr = new Node[3];
  }
  static void arrSetBody() {
    Array.set(objArr, 0, buildNodes(LEN, 40));
    Array.set(objArr, 2, buildBig(41));
    Array.set(nodeArr, 1, buildNodes(LEN, 42));
  }
  static void arrSetVerify() {
    String w = "Array.set";
    verifyNodes((Node) objArr[0], LEN, 40, w);
    promotedNodes((Node) objArr[0], LEN, w);
    if (objArr[1] != null) throw fail(w, "an untouched slot is not null");
    verifyBig((long[]) objArr[2], 41, w);
    promoted(objArr[2], w);
    if (nodeArr[0] != null || nodeArr[2] != null) throw fail(w, "an untouched slot is not null");
    verifyNodes(nodeArr[1], LEN, 42, w);
    promotedNodes(nodeArr[1], LEN, w);
  }

  public static void main(String[] args) {
    warmUp();
    scenario("AtomicReference set", TestScratchEscapeAtomics::setPrepare,
             TestScratchEscapeAtomics::setBody, TestScratchEscapeAtomics::setVerify);
    scenario("AtomicReference compareAndSet", TestScratchEscapeAtomics::casPrepare,
             TestScratchEscapeAtomics::casBody, TestScratchEscapeAtomics::casVerify);
    scenario("AtomicReference getAndSet", TestScratchEscapeAtomics::getAndSetPrepare,
             TestScratchEscapeAtomics::getAndSetBody, TestScratchEscapeAtomics::getAndSetVerify);
    scenario("AtomicReferenceArray", TestScratchEscapeAtomics::arrayPrepare,
             TestScratchEscapeAtomics::arrayBody, TestScratchEscapeAtomics::arrayVerify);
    scenario("VarHandle", TestScratchEscapeAtomics::vhPrepare,
             TestScratchEscapeAtomics::vhBody, TestScratchEscapeAtomics::vhVerify);
    scenario("Field.set", TestScratchEscapeAtomics::fieldPrepare,
             TestScratchEscapeAtomics::fieldBody, TestScratchEscapeAtomics::fieldVerify);
    scenario("Array.set", TestScratchEscapeAtomics::arrSetPrepare,
             TestScratchEscapeAtomics::arrSetBody, TestScratchEscapeAtomics::arrSetVerify);
  }
}
