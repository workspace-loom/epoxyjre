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

#include "gc/epoxy/epoxyHeap.hpp"
#include "gc/epoxy/epoxyPage.hpp"
#include "gc/epoxy/epoxyPageAllocator.hpp"
#include "gc/epoxy/epoxyRegion.hpp"
#include "gc/epoxy/epoxyScopes.hpp"
#include "gc/epoxy/epoxyThreadLocalData.hpp"
#include "gc/shared/gc_globals.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/method.hpp"
#include "oops/oop.inline.hpp"
#include "oops/symbol.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/os.hpp"
#include "utilities/growableArray.hpp"

struct EpoxyScopeEntry {
  char* _class_name;   // internal form, with slashes
  char* _method_name;
  char* _descriptor;   // null to match every overload
};

static GrowableArrayCHeap<EpoxyScopeEntry, mtGC>* _entries = nullptr;

static char* trim(char* s) {
  while (*s == ' ' || *s == '\t') s++;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) {
    s[--n] = '\0';
  }
  return s;
}

void EpoxyScopes::initialize() {
  if (EpoxyScopeFile == nullptr || EpoxyScopeFile[0] == '\0') {
    return;
  }
  FILE* file = os::fopen(EpoxyScopeFile, "r");
  if (file == nullptr) {
    vm_exit_during_initialization("Could not open EpoxyScopeFile", EpoxyScopeFile);
  }
  _entries = new GrowableArrayCHeap<EpoxyScopeEntry, mtGC>(16);

  char line[1024];
  int line_number = 0;
  while (fgets(line, sizeof(line), file) != nullptr) {
    line_number++;
    char* text = trim(line);
    if (text[0] == '\0' || text[0] == '#') {
      continue;
    }
    // package.Class.method or package.Class.method(descriptor)return
    char* descriptor = strchr(text, '(');
    char* name_end = (descriptor != nullptr) ? descriptor : text + strlen(text);
    char* dot = nullptr;
    for (char* p = text; p < name_end; p++) {
      if (*p == '.') dot = p;
    }
    if (dot == nullptr || dot == text || dot + 1 == name_end) {
      fclose(file);
      char message[256];
      jio_snprintf(message, sizeof(message), "EpoxyScopeFile line %d is not package.Class.method", line_number);
      vm_exit_during_initialization(message, EpoxyScopeFile);
    }
    EpoxyScopeEntry entry;
    entry._descriptor = (descriptor != nullptr) ? os::strdup(trim(descriptor), mtGC) : nullptr;
    if (descriptor != nullptr) {
      *descriptor = '\0';
    }
    *dot = '\0';
    entry._method_name = os::strdup(trim(dot + 1), mtGC);
    entry._class_name = os::strdup(text, mtGC);
    for (char* p = entry._class_name; *p != '\0'; p++) {
      if (*p == '.') *p = '/';
    }
    _entries->append(entry);
    log_info(gc, init)("Epoxy scope: %s.%s%s", entry._class_name, entry._method_name,
                       entry._descriptor != nullptr ? entry._descriptor : "");
  }
  fclose(file);
}

bool EpoxyScopes::has_scopes() {
  return _entries != nullptr && _entries->length() > 0;
}

void EpoxyScopes::mark_if_listed(Method* method) {
  if (!has_scopes() || method->is_native() || method->is_abstract()) {
    return;
  }
  Symbol* class_name = method->method_holder()->name();
  Symbol* method_name = method->name();
  for (int i = 0; i < _entries->length(); i++) {
    const EpoxyScopeEntry& e = _entries->at(i);
    if (method_name->equals(e._method_name) &&
        class_name->equals(e._class_name) &&
        (e._descriptor == nullptr || method->signature()->equals(e._descriptor))) {
      // Only the interpreter opens and closes the region.
      method->set_not_compilable_quietly("Epoxy scope", CompLevel_all);
      method->set_not_osr_compilable_quietly("Epoxy scope", CompLevel_all);
      method->set_dont_inline(true);
      method->set_epoxy_scope(true);
      return;
    }
  }
}

void EpoxyScopes::enter(JavaThread* thread, const frame& activation) {
  intptr_t* frame_fp = activation.fp();
  EpoxyHeap* heap = EpoxyHeap::heap();
  log_trace(gc, region)("Epoxy scope enter: frame " PTR_FORMAT, p2i(frame_fp));
  if (heap->enter_scratch(thread)) {
    // Remember which activation opened the region, so that its exit, and
    // only its exit, releases it.
    EpoxyThreadLocalData::scratch(thread)->set_scope_frame(frame_fp);
  }
}

void EpoxyScopes::exit(JavaThread* thread, const frame& activation, oopDesc* result) {
  intptr_t* frame_fp = activation.fp();
  EpoxyRegion* region = EpoxyThreadLocalData::scratch(thread);
  log_trace(gc, region)("Epoxy scope exit: frame " PTR_FORMAT ", innermost region opened by " PTR_FORMAT,
                        p2i(frame_fp), p2i(region != nullptr ? region->scope_frame() : nullptr));
  if (region == nullptr || region->scope_frame() != frame_fp) {
    return;  // this activation did not open a region (entering was refused)
  }
  EpoxyHeap* heap = EpoxyHeap::heap();
  EpoxyPageAllocator* pages = heap->page_allocator();

  // Only two references can leave through the stack: the value being
  // returned, and an exception in flight. Each is moved, with everything
  // unpromoted it reaches, into the enclosing region (the global space if
  // there is none), so that it lives as long as its receiver and no page of
  // this region is kept for it.
  EpoxyRegion* target = region->parent();
  oop moved_result = heap->evacuate(region, cast_to_oop(result), target);
  thread->set_vm_result_oop(heap->evacuate(region, thread->vm_result_oop(), target));
  if (thread->has_pending_exception()) {
    oop moved_exception = heap->evacuate(region, thread->pending_exception(), target);
    thread->set_pending_exception(moved_exception, thread->exception_file(), thread->exception_line());
  }
  if (result != nullptr) {
    // The interpreter takes the returned reference from here.
    thread->set_vm_result_oop(moved_result);
  }

  // The activation's own slots (expression stack, monitors, locals) die with
  // it and may still refer into the region; the verifier leaves them alone.
  address low = (address)activation.sp();
  address high = (address)activation.fp();
  if (activation.interpreter_frame_method()->max_locals() > 0) {
    high = MAX2(high, (address)activation.interpreter_frame_local_at(0));
  }
  heap->exit_scratch(thread, low, high);
}
