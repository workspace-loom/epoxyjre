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

#include "gc/epoxy/epoxyProfile.hpp"
#include "gc/shared/gc_globals.hpp"
#include "memory/resourceArea.hpp"
#include "oops/klass.hpp"
#include "runtime/atomicAccess.hpp"
#include "runtime/os.hpp"
#include "runtime/thread.hpp"
#include "utilities/ostream.hpp"

struct EpoxyProfileEntry {
  const void* key;     // Klass*, or a C-heap copy of a thread name
  size_t      count;
  size_t      bytes;
};

static const size_t KlassSlots = 16384;   // power of two
static const size_t ThreadSlots = 256;
static EpoxyProfileEntry _klasses[KlassSlots];
static EpoxyProfileEntry _threads[ThreadSlots];
static size_t _thread_count = 0;
static size_t _klass_overflow_bytes = 0;
static volatile int _spin = 0;

// A spin lock: these are called from leaf calls made by compiled code, where
// no VM mutex may be taken.
class EpoxyProfileLocker {
public:
  EpoxyProfileLocker() {
    while (AtomicAccess::cmpxchg(&_spin, 0, 1) != 0) {
      os::naked_yield();
    }
  }
  ~EpoxyProfileLocker() { AtomicAccess::release_store(&_spin, 0); }
};

static bool counting() {
  return EpoxyProfileDelay == 0 || os::elapsedTime() >= (double)EpoxyProfileDelay;
}

void EpoxyProfiler::note_promoted(Klass* klass, size_t words) {
  if (!counting()) return;
  EpoxyProfileLocker lock;
  size_t slot = ((uintptr_t)klass >> 3) & (KlassSlots - 1);
  for (size_t probes = 0; probes < KlassSlots; probes++) {
    EpoxyProfileEntry* e = &_klasses[slot];
    if (e->key == nullptr) {
      e->key = klass;
    }
    if (e->key == klass) {
      e->count++;
      e->bytes += words * HeapWordSize;
      return;
    }
    slot = (slot + 1) & (KlassSlots - 1);
  }
  _klass_overflow_bytes += words * HeapWordSize;
}

void EpoxyProfiler::note_global(Thread* thread, size_t words) {
  if (!counting()) return;
  ResourceMark rm;
  const char* name = thread->name();
  char key[64];
  size_t n = 0;
  for (const char* p = name; *p != 0 && n < sizeof(key) - 1; p++) {
    if (*p < '0' || *p > '9') {
      key[n++] = *p;
    }
  }
  key[n] = 0;

  EpoxyProfileLocker lock;
  for (size_t i = 0; i < _thread_count; i++) {
    if (strcmp((const char*)_threads[i].key, key) == 0) {
      _threads[i].count++;
      _threads[i].bytes += words * HeapWordSize;
      return;
    }
  }
  size_t i = _thread_count < ThreadSlots ? _thread_count++ : ThreadSlots - 1;
  if (_threads[i].key == nullptr) {
    _threads[i].key = os::strdup(i == ThreadSlots - 1 ? "(other)" : key, mtGC);
  }
  _threads[i].count++;
  _threads[i].bytes += words * HeapWordSize;
}

static int by_bytes(const void* a, const void* b) {
  size_t x = ((const EpoxyProfileEntry*)a)->bytes;
  size_t y = ((const EpoxyProfileEntry*)b)->bytes;
  return x < y ? 1 : (x > y ? -1 : 0);
}

void EpoxyProfiler::print() {
  ResourceMark rm;
  EpoxyProfileLocker lock;

  size_t n = 0;
  size_t total = _klass_overflow_bytes;
  EpoxyProfileEntry* sorted = NEW_RESOURCE_ARRAY(EpoxyProfileEntry, KlassSlots);
  for (size_t i = 0; i < KlassSlots; i++) {
    if (_klasses[i].key != nullptr) {
      sorted[n++] = _klasses[i];
      total += _klasses[i].bytes;
    }
  }
  qsort(sorted, n, sizeof(EpoxyProfileEntry), by_bytes);
  tty->print_cr("Epoxy profile: promoted %zu MB in %zu classes", total / M, n);
  for (size_t i = 0; i < n && i < 40; i++) {
    tty->print_cr("  %10zu KB %10zu objects  %s", sorted[i].bytes / K, sorted[i].count,
                  ((Klass*)sorted[i].key)->external_name());
  }

  qsort(_threads, _thread_count, sizeof(EpoxyProfileEntry), by_bytes);
  size_t global = 0;
  for (size_t i = 0; i < _thread_count; i++) {
    global += _threads[i].bytes;
  }
  tty->print_cr("Epoxy profile: %zu MB handed out in the global space, by thread", global / M);
  for (size_t i = 0; i < _thread_count && i < 40; i++) {
    tty->print_cr("  %10zu KB %10zu requests  %s", _threads[i].bytes / K, _threads[i].count,
                  (const char*)_threads[i].key);
  }
}
