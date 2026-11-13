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
 * @test TestScratchEscapeThreads
 * @requires vm.gc.Epoxy & vm.debug
 * @summary Region objects handed to other threads survive the exit of a scratch region
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox gc.epoxy.ScratchTestSupport
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit gc.epoxy.TestScratchEscapeThreads
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:-UseTLAB gc.epoxy.TestScratchEscapeThreads
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *                   -XX:+UseEpoxy -Xint -Xmx256m -XX:+EpoxyVerifyScratchExit -XX:EpoxyPageSize=64k gc.epoxy.TestScratchEscapeThreads
 */

import java.util.ArrayList;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;

import static gc.epoxy.ScratchTestSupport.*;

public class TestScratchEscapeThreads {

  // ------------------------------------------------------------------------------------------
  // 1. Items offered from inside a region to a running consumer through a pre-region queue.

  static final int ITEMS = 1000;        // ITEMS - 1 nodes and one big array
  static final LinkedBlockingQueue<Object> QUEUE = new LinkedBlockingQueue<>();
  static final Node POISON = new Node(-1, 0);     // allocated outside any region
  static ArrayList<Object> received;              // the consumer's own static list
  static volatile String consumerError;
  static CountDownLatch ready, taken;

  static void verifyReceived(String w) {
    if (received.size() != ITEMS) throw fail(w, "received " + received.size() + " items");
    for (int i = 0; i < ITEMS - 1; i++) {
      Object o = received.get(i);
      if (!(o instanceof Node)) throw fail(w, "item " + i + " is not a node");
      verifyNode((Node) o, i, 50, w);
      promoted(o, w);
    }
    verifyBig((long[]) received.get(ITEMS - 1), 50, w);
    promoted(received.get(ITEMS - 1), w);
  }

  static final class Consumer extends Thread {
    @Override
    public void run() {
      try {
        ready.countDown();
        for (;;) {
          Object o = QUEUE.take();
          if (o == POISON) break;
          received.add(o);
          taken.countDown();
        }
        verifyReceived("consumer's view");
      } catch (Throwable t) {
        consumerError = t.toString();
      }
    }
  }

  static void offerAll() {
    for (int i = 0; i < ITEMS - 1; i++) QUEUE.offer(new Node(i, 50));
    QUEUE.offer(buildBig(50));
  }

  static void handoff(boolean real) throws Exception {
    realRun = real;
    received = new ArrayList<>();
    consumerError = null;
    ready = new CountDownLatch(1);
    taken = new CountDownLatch(ITEMS);
    Consumer consumer = new Consumer();
    consumer.start();
    awaitLatch(ready);                  // the consumer is running and about to block in take()

    if (real) enter();
    offerAll();
    awaitLatch(taken);                  // the consumer has taken and stored everything
    if (real) {
      exit();
      churn();                          // only now is any wrongly freed memory overwritten
    }

    QUEUE.put(POISON);                  // the consumer verifies its own view, then ends
    consumer.join();
    if (consumerError != null) throw fail("queue hand-over", "consumer failed: " + consumerError);
    verifyReceived("main's view");
    System.out.println("queue hand-over: ok");
  }

  // ------------------------------------------------------------------------------------------
  // 2. A thread created and started inside the region, writing into a pre-region map.

  static final ConcurrentHashMap<String, Object> RESULTS = new ConcurrentHashMap<>();

  static final class Worker implements Runnable {
    final Node list;
    final long[] big;

    Worker(Node list, long[] big) { this.list = list; this.big = big; }

    @Override
    public void run() {
      long sum = 0;
      for (Node n = list; n != null; n = n.next) sum += n.id;
      RESULTS.put("sum", Long.valueOf(sum));
      RESULTS.put("list", list);
      RESULTS.put("name", list.name);
      RESULTS.put("big", big);
    }
  }

  static void spawnPrepare() { RESULTS.clear(); }
  static void spawnBody() {
    Thread t = new Thread(new Worker(buildNodes(100, 51), buildBig(51)));
    t.start();
    joinThread(t);
  }
  static void spawnVerify() {
    String w = "thread started in the region";
    check(RESULTS.size() == 4, "worker did not store all results");
    if (((Long) RESULTS.get("sum")).longValue() != 4950L) throw fail(w, "wrong checksum");
    verifyNodes((Node) RESULTS.get("list"), 100, 51, w);
    promotedNodes((Node) RESULTS.get("list"), 100, w);
    if (!name(0, 51).equals(RESULTS.get("name"))) throw fail(w, "wrong name " + RESULTS.get("name"));
    promoted(RESULTS.get("name"), w);
    verifyBig((long[]) RESULTS.get("big"), 51, w);
    promoted(RESULTS.get("big"), w);
  }

  // ------------------------------------------------------------------------------------------
  // 3. An in-region object used as a lock, under contention from a second thread that reaches
  //    it only through the static it was stored in first.

  static final int ROUNDS = 500;
  static final int HITS = 20_000;

  static final class Lock {
    int hits;
    int count;
    int turn;
  }

  static Lock lock;
  static CountDownLatch go, finished;

  static void waitOn(Lock l) {
    try {
      l.wait();
    } catch (InterruptedException e) {
      throw new IllegalStateException(e);
    }
  }

  static final class Contender extends Thread {
    @Override
    public void run() {
      awaitLatch(go);
      Lock l = lock;
      for (int i = 0; i < HITS; i++) {
        synchronized (l) { l.hits++; }
      }
      for (int r = 0; r < ROUNDS; r++) {
        synchronized (l) {
          while (l.turn != 1) waitOn(l);
          l.count += 2;
          l.turn = 0;
          l.notifyAll();
        }
      }
      finished.countDown();
    }
  }

  static void lockBody() {
    Lock l = new Lock();
    lock = l;                           // escapes before the other thread can see it
    go.countDown();
    for (int i = 0; i < HITS; i++) {
      synchronized (l) { l.hits++; }
    }
    for (int r = 0; r < ROUNDS; r++) {
      synchronized (l) {
        while (l.turn != 0) waitOn(l);
        l.count++;
        l.turn = 1;
        l.notifyAll();
      }
    }
    awaitLatch(finished);
  }

  static void lockFlow(boolean real) {
    realRun = real;
    lock = null;
    go = new CountDownLatch(1);
    finished = new CountDownLatch(1);
    Contender contender = new Contender();
    contender.start();

    if (real) enter();
    lockBody();
    if (real) {
      exit();
      churn();
    }

    joinThread(contender);
    String w = "lock under contention";
    synchronized (lock) {
      if (lock.hits != 2 * HITS) throw fail(w, "hits is " + lock.hits);
      if (lock.count != 3 * ROUNDS) throw fail(w, "count is " + lock.count);
      if (lock.turn != 0) throw fail(w, "turn is " + lock.turn);
    }
    promoted(lock, w);
    System.out.println("lock under contention: ok");
  }

  public static void main(String[] args) throws Exception {
    warmUp();
    handoff(false);                     // warm-up outside any region
    handoff(true);
    scenario("thread started in the region", TestScratchEscapeThreads::spawnPrepare,
             TestScratchEscapeThreads::spawnBody, TestScratchEscapeThreads::spawnVerify);
    lockFlow(false);
    lockFlow(true);
  }
}
