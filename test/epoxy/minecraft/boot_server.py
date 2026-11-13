#!/usr/bin/env python3
#
# Copyright (c) 2026, the EpoxyJRE authors. All rights reserved.
# DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
#
# This code is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 only, as
# published by the Free Software Foundation.
#
# This code is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# version 2 for more details (a copy is included in the LICENSE file that
# accompanied this code).
#
"""Acceptance test: boot a Minecraft dedicated server on a JDK image.

The server jar is not part of this repository; pass your own copy. By running
this script you accept the Minecraft EULA (https://aka.ms/MinecraftEULA) for
the temporary server it starts.

Exit status is 0 only if the expectation given with --expect holds:

  oom    the VM logged "Using Epoxy", the server finished booting, and the VM
         then terminated with java.lang.OutOfMemoryError (a heap that frees
         nothing must end this way once chunks are generated)
  alive  the VM logged "Using Epoxy", the server finished booting, and it was
         still running --seconds after the chunk load was requested
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

PROPERTIES = """\
level-seed=epoxy
online-mode=false
white-list=false
server-port=25599
spawn-protection=0
pause-when-empty-seconds=-1
"""

HEAP_LINE = re.compile(r"Heap: .* committed, (\d+)([KMG]) \(")
UNIT_MB = {"K": 1 / 1024, "M": 1, "G": 1024}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jdk", required=True, help="JDK image directory (contains bin/java)")
    ap.add_argument("--server-jar", default="spike/server/server.jar", help="path to a Minecraft server jar")
    ap.add_argument("--heap", default="4g", help="heap size, as for -Xmx")
    ap.add_argument("--vm-flag", action="append", default=[], help="extra VM flag; repeatable")
    ap.add_argument("--expect", choices=["oom", "alive"], required=True)
    ap.add_argument("--seconds", type=int, default=120, help="for --expect alive: how long it must survive")
    ap.add_argument("--sprint-ticks", type=int, default=0,
                    help="for --expect alive: after --settle seconds run this many ticks as fast as the "
                         "server can (tick sprint) and pass as soon as they are done; --seconds is then "
                         "the time limit")
    ap.add_argument("--settle", type=int, default=20, help="seconds between the chunk load and the sprint")
    ap.add_argument("--boot-timeout", type=int, default=600)
    ap.add_argument("--oom-timeout", type=int, default=900, help="for --expect oom: give up after this long")
    args = ap.parse_args()

    java = os.path.join(os.path.abspath(args.jdk), "bin", "java")
    jar = os.path.abspath(args.server_jar)
    for path in (java, jar):
        if not os.path.isfile(path):
            print(f"FAIL: not found: {path}")
            return 2

    workdir = tempfile.mkdtemp(prefix="epoxy-mc-")
    with open(os.path.join(workdir, "eula.txt"), "w") as f:
        f.write("eula=true\n")
    with open(os.path.join(workdir, "server.properties"), "w") as f:
        f.write(PROPERTIES)

    cmd = [java, f"-Xms{args.heap}", f"-Xmx{args.heap}", "-Xlog:gc"] + args.vm_flag + ["-jar", jar, "nogui"]
    start = time.time()
    proc = subprocess.Popen(cmd, cwd=workdir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)

    state = {"epoxy": False, "oom": False, "used_mb": 0.0, "sprint": None}
    booted = threading.Event()
    log_path = os.path.join(workdir, "console.log")

    def reader():
        with open(log_path, "w") as log:
            for line in proc.stdout:
                log.write(line)
                if "Using Epoxy" in line:
                    state["epoxy"] = True
                if "java.lang.OutOfMemoryError" in line:
                    state["oom"] = True
                if "Done (" in line:
                    booted.set()
                if "Sprint completed" in line:
                    state["sprint"] = line.strip()
                m = HEAP_LINE.search(line)
                if m:
                    state["used_mb"] = float(m.group(1)) * UNIT_MB[m.group(2)]
        booted.set()

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    def finish(ok, why):
        if proc.poll() is None:
            try:
                proc.stdin.write("stop\n")
                proc.stdin.flush()
                proc.wait(60)
            except Exception:
                proc.kill()
        thread.join(10)
        print(f"{'PASS' if ok else 'FAIL'}: {why}")
        print(f"survived {time.time() - start:.0f} s, last reported heap use {state['used_mb']:.0f} MB, "
              f"exit code {proc.returncode}")
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"console log kept at {log_path}")
        return 0 if ok else 1

    booted.wait(args.boot_timeout)
    if proc.poll() is not None or not booted.is_set():
        thread.join(10)
        if not state["epoxy"]:
            return finish(False, 'VM did not log "Using Epoxy" and the server did not finish booting')
        return finish(False, "server did not finish booting")

    # Generate and keep loaded 256 chunks: on a heap that frees nothing this
    # allocates several gigabytes within seconds.
    proc.stdin.write("forceload add -128 -128 127 127\n")
    proc.stdin.flush()

    deadline = time.time() + (args.seconds if args.expect == "alive" else args.oom_timeout)
    sprinting = args.expect == "alive" and args.sprint_ticks > 0
    sprint_at = time.time() + args.settle if sprinting else None
    while time.time() < deadline and proc.poll() is None:
        if sprint_at is not None and time.time() >= sprint_at:
            state["used_before_sprint_mb"] = state["used_mb"]
            proc.stdin.write(f"tick sprint {args.sprint_ticks}\n")
            proc.stdin.flush()
            sprint_at = None
        if sprinting and state["sprint"]:
            break
        time.sleep(0.5)
    exited = proc.poll() is not None
    if exited:
        thread.join(10)

    if not state["epoxy"]:
        return finish(False, 'VM did not log "Using Epoxy"')
    if args.expect == "oom":
        if exited and state["oom"]:
            return finish(True, "server booted on Epoxy and ran until the heap was exhausted")
        if exited:
            return finish(False, "VM exited without java.lang.OutOfMemoryError")
        return finish(False, f"heap was not exhausted within {args.oom_timeout} s")
    if exited:
        return finish(False, f"server exited before {args.seconds} s had passed")
    if sprinting:
        if not state["sprint"]:
            return finish(False, f"sprint of {args.sprint_ticks} ticks did not complete within {args.seconds} s")
        print(state["sprint"])
        print(f"heap use before sprint {state.get('used_before_sprint_mb', 0):.0f} MB")
        return finish(True, f"server booted on Epoxy and survived a sprint of {args.sprint_ticks} ticks")
    return finish(True, f"server booted on Epoxy and was still running after {args.seconds} s")


if __name__ == "__main__":
    sys.exit(main())
