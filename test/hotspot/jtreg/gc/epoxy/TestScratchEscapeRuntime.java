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
 * @test TestScratchEscapeRuntime
 * @requires vm.gc.Epoxy & vm.debug
 * @summary Objects the VM itself keeps (interned strings, classes, exceptions, ...) survive the exit of a scratch region
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit gc.epoxy.TestScratchEscapeRuntime
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:-UseTLAB gc.epoxy.TestScratchEscapeRuntime
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:EpoxyPageSize=64k gc.epoxy.TestScratchEscapeRuntime
 */

import java.lang.ref.SoftReference;
import java.lang.ref.WeakReference;
import java.util.function.IntSupplier;
import java.util.function.Supplier;

import static gc.epoxy.ScratchTestSupport.*;

public class TestScratchEscapeRuntime {

  // ------------------------------------------------------------------------------------------
  // 1. String.intern() of strings built in the region. The strings differ from run to run, so
  //    the warm-up cannot have interned the ones the real run uses.

  static final int SYMS = 200;
  static int run;

  static String sym(int i) {
    return "sym-".concat(Integer.toString(run)).concat("-").concat(Integer.toString(i));
  }

  static void internPrepare() { run++; }
  static void internBody() {
    for (int i = 0; i < SYMS; i++) sym(i).intern();
  }
  static void internVerify() {
    String w = "String.intern";
    for (int i = 0; i < SYMS; i++) {
      String t = sym(i);                          // equal contents, built outside any region
      String a = t.intern();
      String b = new String(t).intern();
      if (a == t) throw fail(w, "intern returned the new string, so " + i + " was not interned earlier");
      if (a != b) throw fail(w, "two interns of equal strings differ for " + i);
      if (!a.equals(t) || a.length() != t.length() || a.hashCode() != t.hashCode()) {
        throw fail(w, "interned string " + i + " is corrupted: " + a);
      }
      promoted(a, w);
    }
  }

  // ------------------------------------------------------------------------------------------
  // 2. A class loaded inside the region. Gen is loaded by GenLoader from its own bytes, so every
  //    loader gets a fresh copy whose static initializer builds a graph.

  public static class Gen {
    public static int inits;
    public static int[][] table = new int[300][];
    public static String[] names = new String[300];
    public static long[] big = new long[100_000];

    static {
      inits++;
      for (int i = 0; i < 300; i++) {
        table[i] = new int[8];
        for (int j = 0; j < 8; j++) table[i][j] = i;
        names[i] = "gen-".concat(Integer.toString(i));
      }
      for (int j = 0; j < big.length; j++) big[j] = j * 31L + 7;
    }

    /** Returns null if the graph is intact, else a description of the damage. */
    public static String verify() {
      if (inits != 1) return "static initializer ran " + inits + " times";
      if (table.length != 300 || names.length != 300 || big.length != 100_000) return "wrong lengths";
      for (int i = 0; i < 300; i++) {
        if (table[i].length != 8) return "table[" + i + "] has a wrong length";
        for (int j = 0; j < 8; j++) {
          if (table[i][j] != i) return "table[" + i + "][" + j + "] is " + table[i][j];
        }
        if (!names[i].equals("gen-".concat(Integer.toString(i)))) return "names[" + i + "] is " + names[i];
      }
      for (int j = 0; j < big.length; j++) {
        if (big[j] != j * 31L + 7) return "big[" + j + "] is " + big[j];
      }
      return null;
    }
  }

  static final String GEN_NAME = "gc.epoxy.TestScratchEscapeRuntime$Gen";
  static final byte[] GEN_BYTES;
  static {
    try (var in = TestScratchEscapeRuntime.class.getResourceAsStream("TestScratchEscapeRuntime$Gen.class")) {
      GEN_BYTES = in.readAllBytes();
    } catch (java.io.IOException e) {
      throw new ExceptionInInitializerError(e);
    }
  }

  static final class GenLoader extends ClassLoader {
    GenLoader() { super(TestScratchEscapeRuntime.class.getClassLoader()); }

    @Override
    protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
      if (!name.equals(GEN_NAME)) return super.loadClass(name, resolve);
      synchronized (getClassLoadingLock(name)) {
        Class<?> c = findLoadedClass(name);
        if (c == null) c = defineClass(name, GEN_BYTES, 0, GEN_BYTES.length);
        return c;
      }
    }
  }

  static GenLoader loader;              // allocated before the region, defines the class inside it
  static GenLoader loaderKeep;          // allocated inside the region, then stored here

  static void verifyGen(GenLoader l, String w) throws Exception {
    // No loader code runs here: the class is found in the loader's dictionary.
    Class<?> c = Class.forName(GEN_NAME, false, l);
    if (c.getClassLoader() != l) throw fail(w, "class has a wrong loader");
    String err = (String) c.getMethod("verify").invoke(null);
    if (err != null) throw fail(w, err);
    promoted(c, w);
    promoted(c.getField("names").get(null), w);
    promoted(c.getField("big").get(null), w);
  }

  static void classPrepare() { loader = new GenLoader(); }
  static void classBody() throws Exception { Class.forName(GEN_NAME, true, loader); }
  static void classVerify() throws Exception { verifyGen(loader, "class loaded in the region"); }

  static void loaderPrepare() { loaderKeep = null; }
  static void loaderBody() throws Exception {
    GenLoader l = new GenLoader();
    Class.forName(GEN_NAME, true, l);
    loaderKeep = l;
  }
  static void loaderVerify() throws Exception {
    promoted(loaderKeep, "class and loader created in the region");
    verifyGen(loaderKeep, "class and loader created in the region");
  }

  // ------------------------------------------------------------------------------------------
  // 3. Exceptions thrown in the region and caught by code that stores them in statics. Their stack
  //    traces are only materialised after the churn.

  static Throwable caught, vmIndex, vmNull, vmCast, vmArith;
  static Object nullObject;
  static int three = 3, zero;

  static void thrower(int depth) {
    if (depth == 0) {
      throw new IllegalStateException("boom-".concat(Integer.toString(depth)), new RuntimeException("cause"));
    }
    thrower(depth - 1);
  }

  static void exceptionPrepare() { caught = vmIndex = vmNull = vmCast = vmArith = null; }
  static void exceptionBody() {
    try {
      thrower(10);
    } catch (IllegalStateException e) {
      e.addSuppressed(new RuntimeException("suppressed"));
      caught = e;
    }
    try {
      int[] a = new int[1];
      a[three] = 1;
    } catch (ArrayIndexOutOfBoundsException e) {
      vmIndex = e;
    }
    try {
      nullObject.hashCode();
    } catch (NullPointerException e) {
      vmNull = e;
    }
    try {
      Object o = "not an Integer";
      Integer i = (Integer) o;
    } catch (ClassCastException e) {
      vmCast = e;
    }
    try {
      int q = 1 / zero;
    } catch (ArithmeticException e) {
      vmArith = e;
    }
  }
  static void exceptionVerify() {
    String w = "exceptions";
    if (!"boom-0".equals(caught.getMessage())) throw fail(w, "message is " + caught.getMessage());
    if (!"cause".equals(caught.getCause().getMessage())) throw fail(w, "cause is " + caught.getCause());
    if (caught.getSuppressed().length != 1 || !"suppressed".equals(caught.getSuppressed()[0].getMessage())) {
      throw fail(w, "suppressed exception is wrong");
    }
    StackTraceElement[] st = caught.getStackTrace();
    int frames = 0;
    for (StackTraceElement e : st) if (e.getMethodName().equals("thrower")) frames++;
    if (frames != 11) throw fail(w, "stack trace has " + frames + " thrower frames, expected 11");
    if (!st[0].getClassName().equals(TestScratchEscapeRuntime.class.getName())) throw fail(w, "wrong top frame " + st[0]);
    promoted(caught, w);
    promoted(caught.getCause(), w);

    if (!"Index 3 out of bounds for length 1".equals(vmIndex.getMessage())) throw fail(w, "index: " + vmIndex.getMessage());
    String npe = vmNull.getMessage();
    if (npe == null || !npe.startsWith("Cannot invoke")) throw fail(w, "NPE message is " + npe);
    String cce = vmCast.getMessage();
    if (cce == null || !cce.startsWith("class java.lang.String cannot be cast to class java.lang.Integer")) {
      throw fail(w, "CCE message is " + cce);
    }
    if (!"/ by zero".equals(vmArith.getMessage())) throw fail(w, "arithmetic: " + vmArith.getMessage());
    for (Throwable t : new Throwable[] { vmIndex, vmNull, vmCast, vmArith }) {
      if (!t.getStackTrace()[0].getMethodName().equals("exceptionBody")) throw fail(w, "wrong top frame of " + t);
      promoted(t, w);
    }
  }

  // ------------------------------------------------------------------------------------------
  // 4. ThreadLocals set in the region; 20 of them force the thread's ThreadLocalMap to resize.

  static ThreadLocal<Node>[] tls;
  static ThreadLocal<Node> tlInit;

  @SuppressWarnings("unchecked")
  static void tlPrepare() {
    tls = (ThreadLocal<Node>[]) new ThreadLocal<?>[20];
    for (int i = 0; i < tls.length; i++) tls[i] = new ThreadLocal<>();
    tlInit = ThreadLocal.withInitial(() -> buildNodes(100, 62));
  }
  static void tlBody() {
    for (int i = 0; i < tls.length; i++) tls[i].set(new Node(i, 61));
    tlInit.get();                                  // the initial value is created in the region
  }
  static void tlVerify() {
    String w = "ThreadLocal";
    for (int i = 0; i < tls.length; i++) {
      verifyNode(tls[i].get(), i, 61, w);
      promoted(tls[i].get(), w);
    }
    verifyNodes(tlInit.get(), 100, 62, w);
    promotedNodes(tlInit.get(), 100, w);
  }

  // ------------------------------------------------------------------------------------------
  // 5. A WeakReference and a SoftReference created in the region, stored together with a strong
  //    reference to the referent.

  static WeakReference<Node> weak;
  static SoftReference<Node> soft;
  static Node strong1, strong2;

  static void refPrepare() { weak = null; soft = null; strong1 = null; strong2 = null; }
  static void refBody() {
    Node a = new Node(1, 70);
    Node b = new Node(2, 71);
    WeakReference<Node> w = new WeakReference<>(a);
    SoftReference<Node> s = new SoftReference<>(b);
    weak = w;
    soft = s;
    strong1 = a;
    strong2 = b;
  }
  static void refVerify() {
    String w = "Weak/SoftReference";
    verifyNode(strong1, 1, 70, w);
    verifyNode(strong2, 2, 71, w);
    if (weak.get() != strong1) throw fail(w, "weak.get() does not return the referent");
    if (soft.get() != strong2) throw fail(w, "soft.get() does not return the referent");
    promoted(weak, w);
    promoted(soft, w);
    promoted(strong1, w);
    promoted(strong2, w);
  }

  // ------------------------------------------------------------------------------------------
  // 6. Object.clone() of pre-region objects inside the region.

  static final class Proto implements Cloneable {
    int id;
    String name;
    int[] data;
    Object[] refs;

    Proto copy() {
      try {
        return (Proto) super.clone();
      } catch (CloneNotSupportedException e) {
        throw new IllegalStateException(e);
      }
    }
  }

  static Proto proto, cloneObj;
  static Object[] protoArr, cloneArr;
  static long[] protoBig, cloneBig;

  static void clonePrepare() {
    proto = new Proto();
    proto.id = 77;
    proto.name = "proto";
    proto.data = new int[] { 1, 2, 3, 4, 5, 6, 7, 8 };
    proto.refs = new Object[] { new Node(1, 80), new Node(2, 80) };
    protoArr = new Object[] { new Node(3, 80), new Node(4, 80), new Node(5, 80), new Node(6, 80) };
    protoBig = buildBig(80);
    cloneObj = null;
    cloneArr = null;
    cloneBig = null;
  }
  static void cloneBody() {
    Proto c = proto.copy();             // the clone is allocated in the region
    c.name = "clone".concat(Integer.toString(c.id));
    c.data = new int[8];
    for (int j = 0; j < 8; j++) c.data[j] = 78;
    cloneObj = c;
    Object[] a = protoArr.clone();
    a[0] = new Node(5, 81);
    cloneArr = a;
    cloneBig = protoBig.clone();
  }
  static void cloneVerify() {
    String w = "Object.clone";
    if (cloneObj == proto) throw fail(w, "clone is the original");
    if (cloneObj.id != 77 || !"clone77".equals(cloneObj.name)) throw fail(w, "clone fields are wrong");
    if (cloneObj.refs != proto.refs) throw fail(w, "clone does not share the original's refs array");
    for (int j = 0; j < 8; j++) {
      if (cloneObj.data[j] != 78) throw fail(w, "clone data[" + j + "] is " + cloneObj.data[j]);
    }
    for (int j = 0; j < 8; j++) {
      if (proto.data[j] != j + 1) throw fail(w, "the original was modified");
    }
    promoted(cloneObj, w);
    promoted(cloneObj.data, w);
    if (cloneArr.length != 4) throw fail(w, "array clone has length " + cloneArr.length);
    verifyNode((Node) cloneArr[0], 5, 81, w);
    promoted(cloneArr[0], w);
    for (int i = 1; i < 4; i++) {
      if (cloneArr[i] != protoArr[i]) throw fail(w, "array clone element " + i + " is not the original's");
    }
    promoted(cloneArr, w);
    if (cloneBig == protoBig) throw fail(w, "big clone is the original");
    verifyBig(cloneBig, 80, w);
    verifyBig(protoBig, 80, w);
    promoted(cloneBig, w);
    notPromoted(proto, w);
  }

  // ------------------------------------------------------------------------------------------
  // 7. A lambda and method references that capture region objects, invoked after the churn.

  static Supplier<String> lambda, boundRef;
  static IntSupplier ints;

  static void lambdaPrepare() { lambda = null; boundRef = null; ints = null; }
  static void lambdaBody() {
    Node n = new Node(3, 90);
    int[] arr = n.data;
    lambda = () -> n.name.concat("!");
    boundRef = n::label;
    ints = () -> arr[0] + arr[7];
  }
  static void lambdaVerify() {
    String w = "lambda";
    if (!name(3, 90).concat("!").equals(lambda.get())) throw fail(w, "lambda returns " + lambda.get());
    if (!name(3, 90).equals(boundRef.get())) throw fail(w, "method reference returns " + boundRef.get());
    if (ints.getAsInt() != 2 * (3 + 90)) throw fail(w, "int lambda returns " + ints.getAsInt());
    promoted(lambda, w);
    promoted(boundRef, w);
    promoted(ints, w);
  }

  public static void main(String[] args) {
    warmUp();
    scenario("String.intern", TestScratchEscapeRuntime::internPrepare,
             TestScratchEscapeRuntime::internBody, TestScratchEscapeRuntime::internVerify);
    scenario("class loaded in the region", TestScratchEscapeRuntime::classPrepare,
             TestScratchEscapeRuntime::classBody, TestScratchEscapeRuntime::classVerify);
    scenario("class loader created in the region", TestScratchEscapeRuntime::loaderPrepare,
             TestScratchEscapeRuntime::loaderBody, TestScratchEscapeRuntime::loaderVerify);
    scenario("exceptions", TestScratchEscapeRuntime::exceptionPrepare,
             TestScratchEscapeRuntime::exceptionBody, TestScratchEscapeRuntime::exceptionVerify);
    scenario("ThreadLocal", TestScratchEscapeRuntime::tlPrepare,
             TestScratchEscapeRuntime::tlBody, TestScratchEscapeRuntime::tlVerify);
    scenario("Weak/SoftReference", TestScratchEscapeRuntime::refPrepare,
             TestScratchEscapeRuntime::refBody, TestScratchEscapeRuntime::refVerify);
    scenario("Object.clone", TestScratchEscapeRuntime::clonePrepare,
             TestScratchEscapeRuntime::cloneBody, TestScratchEscapeRuntime::cloneVerify);
    scenario("lambda and method reference", TestScratchEscapeRuntime::lambdaPrepare,
             TestScratchEscapeRuntime::lambdaBody, TestScratchEscapeRuntime::lambdaVerify);
  }
}
