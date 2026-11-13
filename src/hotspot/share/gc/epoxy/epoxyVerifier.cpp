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
 */


#include "utilities/macros.hpp"

#ifdef ASSERT

#include "classfile/classLoaderData.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "code/codeCache.hpp"
#include "code/nmethod.hpp"
#include "gc/epoxy/epoxyHeap.hpp"
#include "gc/epoxy/epoxyPage.hpp"
#include "gc/epoxy/epoxyPageAllocator.hpp"
#include "gc/epoxy/epoxyRegion.hpp"
#include "gc/epoxy/epoxyVerifier.hpp"
#include "gc/shared/oopStorage.inline.hpp"
#include "gc/shared/oopStorageSet.inline.hpp"
#include "logging/log.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/memRegion.hpp"
#include "memory/resourceArea.hpp"
#include "oops/access.inline.hpp"
#include "oops/klass.hpp"
#include "oops/method.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/threads.hpp"
#include "runtime/vmOperation.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/debug.hpp"
#include "utilities/enumIterator.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/ostream.hpp"

// Where the reference being looked at was found.
enum EpoxyRootKind {
  EpoxyRootThread,      // a thread's stack, handles or other thread-local oops
  EpoxyRootOopStorage,  // a slot of an OopStorage (strong or weak)
  EpoxyRootCLD,         // an oop held by a class loader data
  EpoxyRootNMethod,     // an oop embedded in an nmethod
  EpoxyRootField        // a field of an object reached by the walk
};

// What is wrong with the reference.
enum EpoxyViolationKind {
  EpoxyViolationUnpromoted,  // points at an unpromoted object of the region being released
  EpoxyViolationNotInHeap    // points at memory that is not in the heap or is in a freed page
};

struct EpoxyViolation {
  EpoxyRootKind      _root_kind;
  EpoxyViolationKind _kind;
  const void*        _slot;      // address of the reference
  oop                _holder;    // for EpoxyRootField: the object holding the slot
  oop                _bad;       // the value of the reference
  Thread*            _thread;    // for EpoxyRootThread
  const char*        _storage;   // for EpoxyRootOopStorage: the storage's name
  ClassLoaderData*   _cld;       // for EpoxyRootCLD
  nmethod*           _nm;        // for EpoxyRootNMethod
};

// Looks at every reference it is applied to. Used both for root slots (after
// one of the set_*_root functions says what kind of root is being scanned)
// and, through oop_iterate, for the fields of every object on the work stack.
class EpoxyVerifyClosure : public BasicOopIterateClosure {
public:
  static const int MaxViolations = 10;

private:
  EpoxyHeap* const                      _heap;
  EpoxyPageAllocator* const             _pages;
  EpoxyRegion* const                    _region;
  const MemRegion                       _reserved;
  CHeapBitMap* const                    _visited;
  GrowableArrayCHeap<oop, mtGC>* const  _stack;

  size_t         _visited_count;
  int            _total;                  // violations found, including the ones not kept
  EpoxyViolation _violations[MaxViolations];

  // The location currently being scanned.
  EpoxyRootKind    _root_kind;
  oop              _holder;
  Thread*          _thread;
  const char*      _storage;
  ClassLoaderData* _cld;
  nmethod*         _nm;

  size_t index_for(oop obj) const {
    return pointer_delta(cast_from_oop<HeapWord*>(obj), _reserved.start()) >> LogMinObjAlignment;
  }

  void record(const void* slot, oop bad, EpoxyViolationKind kind) {
    if (_total < MaxViolations) {
      EpoxyViolation& v = _violations[_total];
      v._root_kind = _root_kind;
      v._kind      = kind;
      v._slot      = slot;
      v._holder    = _holder;
      v._bad       = bad;
      v._thread    = _thread;
      v._storage   = _storage;
      v._cld       = _cld;
      v._nm        = _nm;
    }
    _total++;
  }

  template <typename T>
  void do_oop_work(T* p) {
    oop obj = RawAccess<>::oop_load(p);
    if (obj == nullptr) {
      return;
    }
    // Never dereference a reference that is not into a live part of the heap.
    if (!_heap->is_in(obj)) {
      record(p, obj, EpoxyViolationNotInHeap);
      return;
    }
    // The bug being hunted: a reference from outside the region (or from a
    // promoted object) to an object the barrier did not promote.
    EpoxyPage* page = _pages->page_for(obj);
    if (page != nullptr && page->owner() == _region && !_heap->is_promoted(obj)) {
      record(p, obj, EpoxyViolationUnpromoted);
      return;
    }
    size_t idx = index_for(obj);
    if (_visited->at(idx)) {
      return;
    }
    _visited->set_bit(idx);
    _visited_count++;
    _stack->push(obj);
  }

public:
  EpoxyVerifyClosure(EpoxyHeap* heap, EpoxyRegion* region,
                     CHeapBitMap* visited, GrowableArrayCHeap<oop, mtGC>* stack) :
      _heap(heap),
      _pages(heap->page_allocator()),
      _region(region),
      _reserved(heap->reserved_region()),
      _visited(visited),
      _stack(stack),
      _visited_count(0),
      _total(0),
      _root_kind(EpoxyRootThread),
      _holder(nullptr),
      _thread(nullptr),
      _storage(nullptr),
      _cld(nullptr),
      _nm(nullptr) {
  }

  void do_oop(oop* p) override       { do_oop_work(p); }
  void do_oop(narrowOop* p) override { do_oop_work(p); }

  // The referent of a java.lang.ref.Reference is an ordinary reference here.
  ReferenceIterationMode reference_iteration_mode() override { return DO_FIELDS; }

  void set_thread_root(Thread* thread) {
    _root_kind = EpoxyRootThread;
    _thread = thread;
  }
  void set_storage_root(const char* name) {
    _root_kind = EpoxyRootOopStorage;
    _storage = name;
  }
  void set_cld_root(ClassLoaderData* cld) {
    _root_kind = EpoxyRootCLD;
    _cld = cld;
  }
  void set_nmethod_root(nmethod* nm) {
    _root_kind = EpoxyRootNMethod;
    _nm = nm;
  }

  // Looks at every field of obj.
  void scan_object(oop obj) {
    _root_kind = EpoxyRootField;
    _holder = obj;
    obj->oop_iterate(this);
  }

  // Follows every object pushed so far, and the ones they push in turn.
  void drain() {
    while (!_stack->is_empty()) {
      scan_object(_stack->pop());
    }
  }

  int total_violations() const    { return _total; }
  size_t visited_count() const    { return _visited_count; }

  void print_violations(Thread* releasing_thread) const {
    int shown = MIN2(_total, MaxViolations);
    ResourceMark rm;
    tty->print_cr("EpoxyVerifier: %d bad reference(s) found while thread \"%s\" releases scratch region \"%s\"; showing %d:",
                  _total, releasing_thread->name(), _region->name(), shown);
    for (int i = 0; i < shown; i++) {
      ResourceMark rm_each;
      const EpoxyViolation& v = _violations[i];
      tty->print("  [%d] ", i);
      switch (v._root_kind) {
        case EpoxyRootThread:
          tty->print("thread stack or thread-local oop of thread \"%s\"", v._thread->name());
          break;
        case EpoxyRootOopStorage:
          tty->print("OopStorage \"%s\"", v._storage != nullptr ? v._storage : "(unnamed)");
          break;
        case EpoxyRootCLD:
          tty->print("class loader data " PTR_FORMAT " (%s)", p2i(v._cld), v._cld->loader_name_and_id());
          break;
        case EpoxyRootNMethod:
          tty->print("nmethod " PTR_FORMAT " (%s)", p2i(v._nm),
                     v._nm->method() != nullptr ? v._nm->method()->external_name() : "no method");
          break;
        case EpoxyRootField:
          tty->print("field of %s at " PTR_FORMAT, v._holder->klass()->external_name(), p2i(v._holder));
          break;
      }
      tty->print(": slot " PTR_FORMAT " holds " PTR_FORMAT, p2i(v._slot), p2i(v._bad));
      if (v._kind == EpoxyViolationUnpromoted) {
        // The page is not freed yet, so the object can be read.
        tty->print_cr(", an unpromoted object of class %s", v._bad->klass()->external_name());
      } else {
        tty->print_cr(", a reference to memory that is not in the heap or is in a freed page");
      }
    }
    if (_total > shown) {
      tty->print_cr("  ... and %d more", _total - shown);
    }
  }
};

// Applies the verifying closure to the roots of one thread.
class EpoxyThreadRootClosure : public ThreadClosure {
  EpoxyVerifyClosure* const _cl;
public:
  EpoxyThreadRootClosure(EpoxyVerifyClosure* cl) : _cl(cl) {}

  void do_thread(Thread* thread) override {
    _cl->set_thread_root(thread);
    // No nmethod closure: nmethod oops are scanned through the code cache.
    thread->oops_do(_cl, nullptr);
  }
};

class EpoxyCLDRootClosure : public CLDClosure {
  EpoxyVerifyClosure* const _cl;
public:
  EpoxyCLDRootClosure(EpoxyVerifyClosure* cl) : _cl(cl) {}

  void do_cld(ClassLoaderData* cld) override {
    _cl->set_cld_root(cld);
    cld->oops_do(_cl, ClassLoaderData::_claim_none);
  }
};

class EpoxyNMethodRootClosure : public NMethodClosure {
  EpoxyVerifyClosure* const _cl;
public:
  EpoxyNMethodRootClosure(EpoxyVerifyClosure* cl) : _cl(cl) {}

  void do_nmethod(nmethod* nm) override {
    _cl->set_nmethod_root(nm);
    nm->oops_do(_cl);
  }
};

// Reuses VMOp_GC_HeapInspection: the VM_OPS_DO list is not extended for a
// debug-only checker. The type only names the operation in logs and statistics.
class VM_EpoxyVerifyRelease : public VM_Operation {
private:
  JavaThread* const  _thread;
  EpoxyRegion* const _region;

public:
  VM_EpoxyVerifyRelease(JavaThread* thread, EpoxyRegion* region) :
      _thread(thread), _region(region) {}

  VMOp_Type type() const override { return VMOp_GC_HeapInspection; }

  void doit() override {
    EpoxyHeap* heap = EpoxyHeap::heap();
    MemRegion reserved = heap->reserved_region();

    // One bit per object alignment unit of the whole heap.
    CHeapBitMap visited((reserved.word_size() >> LogMinObjAlignment) + 1, mtGC);
    GrowableArrayCHeap<oop, mtGC> stack;
    EpoxyVerifyClosure cl(heap, _region, &visited, &stack);

    // Threads: every JavaThread and the VM thread, the same set as
    // Threads::oops_do, but one at a time so a slot can be blamed on a thread.
    {
      EpoxyThreadRootClosure thread_cl(&cl);
      Threads::java_threads_do(&thread_cl);
      thread_cl.do_thread(VMThread::vm_thread());
    }

    // Every OopStorage, strong and weak. A weak slot that still holds a
    // pointer into a page about to be freed is a violation too; null slots
    // are skipped by the closure.
    for (OopStorage* storage : OopStorageSet::Range<OopStorageSet::Id>()) {
      cl.set_storage_root(storage->name());
      storage->oops_do(&cl);
    }

    // Class loader data: the holder oop and the handles of every loader.
    {
      EpoxyCLDRootClosure cld_cl(&cl);
      ClassLoaderDataGraph::cld_do(&cld_cl);
    }

    // Code cache: oops embedded in every nmethod, not only the ones that are
    // on a stack (Threads::oops_do is given no nmethod closure above). The
    // nmethods are not marked or disarmed: this is a read-only walk.
    {
      EpoxyNMethodRootClosure nm_cl(&cl);
      CodeCache::nmethods_do(&nm_cl);
    }

    // Everything reachable from the roots.
    cl.drain();

    if (cl.total_violations() > 0) {
      cl.print_violations(_thread);
      fatal("EpoxyVerifier: %d reference(s) into a scratch region being released point at unpromoted objects",
            cl.total_violations());
    }
    log_debug(gc, verify)("Epoxy scratch exit verified: %zu objects visited", cl.visited_count());
  }
};

void EpoxyVerifier::verify_release(JavaThread* thread, EpoxyRegion* region) {
  assert(thread == JavaThread::current(), "must be the releasing thread");
  assert(region != nullptr && region->is_scratch(), "must release a scratch region");
  VM_EpoxyVerifyRelease op(thread, region);
  VMThread::execute(&op);
}

#endif // ASSERT
