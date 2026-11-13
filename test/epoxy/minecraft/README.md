# Minecraft acceptance test for Epoxy

`boot_server.py` boots a Minecraft dedicated server on a JDK image built from
this repository and checks what happens to it.

The server jar is not in this repository. Download the version you want to
test from Mojang and pass its path with `--server-jar`. Running the script
accepts the [Minecraft EULA](https://aka.ms/MinecraftEULA) for the temporary
server it starts.

## Usage

```
python3 test/epoxy/minecraft/boot_server.py \
    --jdk build/linux-x86_64-server-release/images/jdk \
    --server-jar /path/to/server.jar \
    --heap 4g \
    --vm-flag=-XX:+UseEpoxy \
    --expect oom
```

The script starts the server in a fresh temporary directory on port 25599,
waits for it to finish booting, then force-loads 256 chunks.

| `--expect` | Passes when |
|---|---|
| `oom` | The VM logged `Using Epoxy`, the server booted, and the VM then ended with `java.lang.OutOfMemoryError`. |
| `alive` | The VM logged `Using Epoxy`, the server booted, and it was still running `--seconds` after the chunk load. |

It prints how long the server survived and the last heap use the VM reported.
On failure the console log is kept and its path is printed.

## Which expectation applies

With no scope file the heap frees nothing and the only correct outcome is
`oom`. With `-XX:EpoxyScopeFile=test/epoxy/minecraft/server-scopes.txt` the
per-tick and per-task garbage is reclaimed and the expectation is `alive`.

For a quick `alive` check add `--sprint-ticks N`: after `--settle` seconds the
script runs N ticks as fast as the server can (`tick sprint`) and passes when
the server reports the sprint done. 6000 ticks are five minutes of game time
and take about ten seconds. `--keep-log` keeps the console log on a pass;
with `--vm-flag=-Xlog:gc+region=info` it holds the region statistics.

Objects that are stored into long-lived objects are kept and never freed, so
memory still grows with every tick; see the results below.

## Reference results

Minecraft 26.3 server, 4 GB heap, 12-core x86-64 Linux machine, 2026-11-12.
Both heaps free nothing, so the times should be close.

| Build | Heap | Result | Survived |
|---|---|---|---|
| fastdebug | Epoxy | pass (`oom`) | 183 s |
| fastdebug | Epsilon | rejected, as intended | 200 s |
| release | Epoxy, before the TLAB and page-tail fixes | pass (`oom`) | 12 s |
| release | Epoxy, final | pass (`oom`) | 14 s |
| release | Epsilon | rejected, as intended | 14 s |

### With scratch regions (release build, 2026-11-13)

256 chunks force-loaded, `server-scopes.txt`, then a sprint. "No regions" is
the table above.

| Change | Heap | Result |
|---|---|---|
| No regions | 4 GB | out of memory after 14 s |
| Tick and worker-task regions, 256-byte lines | 4 GB | out of memory after 31 s |
| 64-byte lines, holes reused by size, regions for start-up, serial executors and packets | 4 GB | 6000 ticks done; out of memory soon after |
| Holes of 128 bytes and more reused | 4 GB | 24000 ticks done (20 minutes of game time), 3.4 GB in use at the end |
| Same | 10 GB | 72000 ticks done (one hour of game time), heap use 2.1 GB before and 5.0 GB after |

Tick time in the sprints was 1.4 to 2.0 ms. The remaining growth, about
40 KB per tick, is data that was stored into long-lived objects and later
dropped by the game; `-XX:+EpoxyProfile` lists it by class.

## HotSpot tier-1 runtime tests on Epoxy

`make test TEST=tier1_runtime JTREG="VM_OPTIONS=-XX:+UseEpoxy -Xmx2g"`, fastdebug,
2026-11-12: 777 tests, 492 passed, 246 skipped, 39 failed.

Read the failures with care: jtreg reuses one VM for many tests, and a heap
that never frees eventually fills that shared VM, so some tests fail only
because of what ran before them. Rerun a failing test alone before trusting it.

### Known failures shared with Epsilon

These fail on Epoxy and on Epsilon for the same reason: they expect memory or
classes to be reclaimed (class unloading, weak references, `System.gc()`), or
they deliberately exhaust the heap and expect an exception rather than an exit.

- `runtime/CheckUnhandledOops/TestOutOfMemory.java`
- `runtime/ClassInitErrors/InitExceptionUnloadTest.java`
- `runtime/ClassInitErrors/TestOutOfMemoryDuringInit.java`
- `runtime/ClassUnload/ConstantPoolDependsTest.java`
- `runtime/ClassUnload/DictionaryDependsTest.java`
- `runtime/ClassUnload/KeepAliveClass.java`
- `runtime/ClassUnload/KeepAliveClassLoader.java`
- `runtime/ClassUnload/KeepAliveObject.java`
- `runtime/ClassUnload/KeepAliveSoftReference.java`
- `runtime/ClassUnload/SuperDependsTest.java`
- `runtime/ClassUnload/UnloadInterfaceTest.java`
- `runtime/ClassUnload/UnloadTestDuringClassLoaderStatsVMOperation.java`
- `runtime/ClassUnload/UnloadTest.java`
- `runtime/ErrorHandling/GenOutOfMemoryError.java`
- `runtime/handshake/AsyncHandshakeWalkStackTest.java`
- `runtime/handshake/HandshakeWalkStackTest.java`
- `runtime/HiddenClasses/GCHiddenClass.java`
- `runtime/InternalApi/ThreadCpuTimesDeadlock.java`
- `runtime/interned/SanityTest.java`
- `runtime/jni/ReturnJNIWeak/ReturnJNIWeak.java`
- `runtime/memory/MultiAllocateNullCheck.java`
- `runtime/Metaspace/DefineClass.java`
- `runtime/reflect/ReflectOutOfMemoryError.java`
- `runtime/SameObject/SameObject.java`
- `runtime/Thread/SetNameAtExit.java`

### Known failure on Epoxy only

- `runtime/os/TestTrimNativeHeapIntervalTablesCleanup.java` runs out of heap at
  its fixed `-Xmx128m`. It compiles classes in a loop and calls `System.gc()`
  between rounds. It fits on Epsilon and not on Epoxy; how much more heap Epoxy
  needs for it has not been measured. Expected to pass once regions reclaim
  memory.

Twelve other tests failed only on Epoxy in the full run and pass now: six pass
when run alone (the shared-VM effect above), five were fixed by giving each
thread a small first TLAB, and one test was taught that Epoxy, like Epsilon,
has no collector to select.
