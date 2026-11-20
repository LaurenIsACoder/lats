# Windows HOST3D BLOB integration

The LATS external BLOB path requires a virglrenderer build that exports
`virgl_renderer_resource_map_fixed()`. Use the matching renderer's LATS release
profile and pass its installation prefix to the launcher.

Build a release LATS against the matching renderer and its private dependency
prefix with:

```sh
scripts/build-host3d-blob-release.sh \
    /path/to/virglrenderer-prefix \
    /path/to/renderer-dependencies
```

The script uses Meson `buildtype=release` in addition to
`--enable-latx-ndebug`; this produces optimization level 3, disables debug
information and assertions, and avoids accidentally benchmarking a debug
configuration.

Run the three controlled configurations with all other settings unchanged:

```sh
export LATS_DISK_IMAGE=/path/to/windows.qcow2
export LATS_BIN=/path/to/lats-x86_64
export LATS_RENDERER_PREFIX=/path/to/virglrenderer-prefix
# Only needed when renderer dependencies are outside the system library path:
export LATS_RENDERER_DEP_PREFIX=/path/to/renderer-dependencies

LATS_BLOB_MODE=off scripts/run-windows-vgpu.sh
LATS_BLOB_MODE=standard scripts/run-windows-vgpu.sh
LATS_BLOB_MODE=external scripts/run-windows-vgpu.sh
```

For a trace that proves which path was exercised, add for example:

```sh
LATS_BLOB_MODE=external \
LATS_TRACE_FILE=/tmp/lats-host3d-blob.trace \
scripts/run-windows-vgpu.sh
```

The trace records blob creation, BAR mapping, fixed dma-buf mapping, explicit
CPU synchronization, and teardown. Tracing is disabled unless
`LATS_TRACE_FILE` is set.

For unattended comparison, all modes can use the same headless settings:

```sh
export LATS_DISPLAY=egl-headless,rendernode=/dev/dri/renderD129
export LATS_VNC_ADDRESS=:5
export LATS_QMP_SOCKET=/tmp/lats-blob.qmp
export LATS_QGA_SOCKET=/tmp/lats-blob.qga
export LATS_TCG_TB_MB=1024
```

When the Windows QEMU Guest Agent is installed, execute a command without an
interactive desktop using `scripts/qga-exec.py`:

```sh
scripts/qga-exec.py --guest-encoding gb18030 /tmp/lats-blob.qga \
    cmd.exe /c ver
```

For images without a guest agent, `scripts/qmp-type.py` can send a key chord or
ASCII text to the active console.  Keep modifier transitions separate, as the
helper does, because some input backends otherwise coalesce Ctrl+A or Win+R.

```sh
scripts/qmp-type.py /tmp/lats-blob.qmp --chord meta_l,r
scripts/qmp-type.py /tmp/lats-blob.qmp --text notepad --enter
```

`standard` advertises resource blobs and the host-visible BAR. `external` also
asks virglrenderer to allocate FD-backed HOST3D storage and map it directly at
the BAR address. Keep host memory at 256 MiB for A/B testing unless BAR pressure
is itself being tested.

Fixed-mapped dma-bufs use the project `RESOURCE_SYNC_BLOB` extension. The host
advertises the feature only when `map_fixed` support is compiled in. The Windows
KMD must negotiate the feature and expose `SupportsHost3DBlobSync`; the UMD must
not select coherent HOST3D storage without that capability.

The extension currently uses feature bit 5 and command `0x020a`. Keep the LATS,
KMD and UMD definitions synchronized and move to the standard command if a
future virtio-gpu specification assigns equivalent semantics.
