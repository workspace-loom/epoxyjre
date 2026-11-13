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
#include "runtime/stackFrameStream.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "gc/shared/markBitMap.inline.hpp"
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
  EpoxyViolationNotInHeap,   // points at memory that is not in the heap or is in a freed page
  EpoxyViolationFromPromoted // a promoted object refers to an unpromoted scratch object of any region
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

  bool _scanning_promoted;

  Thread* _releasing_thread;

  address _skip_low;

  address _skip_high;
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
    if (_root_kind == EpoxyRootThread && _thread == _releasing_thread &&
        (address)p >= _skip_low && (address)p <= _skip_high && _skip_low != nullptr) {
      return;  // a slot of the activation that is being left
    }
    if (_scanning_promoted) {
      // A promoted object outlives every region: nothing it refers to may
      // be an unpromoted scratch object, whichever region that is in.
      if (!_heap->is_in(obj)) {
        record(p, obj, EpoxyViolationNotInHeap);
      } else if (_pages->is_scratch(cast_from_oop<void*>(obj)) && !_heap->is_promoted(obj)) {
        record(p, obj, EpoxyViolationFromPromoted);
      }
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
      _scanning_promoted(false),
      _releasing_thread(nullptr),
      _skip_low(nullptr),
      _skip_high(nullptr),
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

  // Checks the fields of one promoted object of the region being released,
  // whether or not anything still refers to it: it stays in the heap, and
  // heap walks visit it, for as long as the VM runs.
  void scan_promoted_object(oop obj) {
    _scanning_promoted = true;
    _root_kind = EpoxyRootField;
    _holder = obj;
    obj->oop_iterate(this);
    _scanning_promoted = false;
  }

  void set_skipped_range(Thread* releasing_thread, address low, address high) {
    _releasing_thread = releasing_thread;
    _skip_low = low;
    _skip_high = high;
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
          if (v._thread->is_Java_thread()) {
            // Say which frame the slot is in, and show the thread's stack once.
            JavaThread* jt = JavaThread::cast(v._thread);
            if (jt->has_last_Java_frame()) {
              bool found = false;
              for (StackFrameStream fst(jt, true /* update */, true /* process_frames */); !fst.is_done(); fst.next()) {
                frame* f = fst.current();
                if ((address)v._slot >= (address)f->sp() && (address)v._slot < (address)f->real_fp()) {
                  tty->cr();
                  tty->print("      in frame: ");
                  f->print_on(tty);
                  found = true;
                  break;
                }
              }
              if (!found) {
                tty->print(" (slot is not inside a frame: a handle or other thread-local reference)");
              }
              if (i == 0) {
                tty->cr();
                tty->print_cr("      stack of that thread:");
                jt->print_stack_on(tty);
              }
            }
          }
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
      if (v._kind == EpoxyViolationFromPromoted) {
        tty->print_cr(", an unpromoted object of class %s referred to by a promoted object", v._bad->klass()->external_name());
      } else if (v._kind == EpoxyViolationUnpromoted) {
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
  const address      _skip_low;
  const address      _skip_high;

public:
  VM_EpoxyVerifyRelease(JavaThread* thread, EpoxyRegion* region, address skip_low, address skip_high) :
      _thread(thread), _region(region), _skip_low(skip_low), _skip_high(skip_high) {}

  VMOp_Type type() const override { return VMOp_GC_HeapInspection; }

  void doit() override {
    EpoxyHeap* heap = EpoxyHeap::heap();
    MemRegion reserved = heap->reserved_region();

    // One bit per object alignment unit of the whole heap.
    CHeapBitMap visited((reserved.word_size() >> LogMinObjAlignment) + 1, mtGC);
    GrowableArrayCHeap<oop, mtGC> stack;
    EpoxyVerifyClosure cl(heap, _region, &visited, &stack);
    cl.set_skipped_range(_thread, _skip_low, _skip_high);

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

    // Every promoted object in what the region allocated in, reachable or
    // not. Promoted objects that earlier regions left in the kept lines of a
    // recycled page were checked when those regions were released.
    const MarkBitMap* marks = heap->promoted_marks();
    for (int i = 0; i < _region->span_count(); i++) {
      EpoxySpan span = _region->span_at(i);
      HeapWord* addr = marks->get_next_marked_addr(span.start, span.top);
      while (addr < span.top) {
        oop obj = cast_to_oop(addr);
        cl.scan_promoted_object(obj);
        addr = marks->get_next_marked_addr(addr + obj->size(), span.top);
      }
    }

    if (cl.total_violations() > 0) {
      cl.print_violations(_thread);
      fatal("EpoxyVerifier: %d reference(s) into a scratch region being released point at unpromoted objects",
            cl.total_violations());
    }
    log_debug(gc, verify)("Epoxy scratch exit verified: %zu objects visited", cl.visited_count());
  }
};

void EpoxyVerifier::verify_release(JavaThread* thread, EpoxyRegion* region, address skip_low, address skip_high) {
  assert(thread == JavaThread::current(), "must be the releasing thread");
  assert(region != nullptr && region->is_scratch(), "must release a scratch region");
  VM_EpoxyVerifyRelease op(thread, region, skip_low, skip_high);
  VMThread::execute(&op);
}

#endif // ASSERT
