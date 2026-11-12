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

Until scratch regions exist, the Epoxy heap allocates from pages and never
frees, so the only correct outcome is `oom`. Once regions reclaim per-tick
garbage the expectation becomes `alive`.

Running with another heap (for example `--vm-flag=-XX:+UseEpsilonGC`) always
fails with `VM did not log "Using Epoxy"`. That is the check that the script
is really testing this heap.

## Reference results

Minecraft 26.3 server, 4 GB heap, 12-core x86-64 Linux machine, 2026-11-12.
Both heaps free nothing, so the times should be close.

| Build | Heap | Result | Survived |
|---|---|---|---|
| fastdebug | Epoxy | pass (`oom`) | 183 s |
| fastdebug | Epsilon | rejected, as intended | 200 s |
| release | Epoxy | pass (`oom`) | 12 s |
| release | Epsilon | rejected, as intended | 14 s |
