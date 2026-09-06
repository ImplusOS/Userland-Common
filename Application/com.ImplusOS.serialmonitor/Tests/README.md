# LogRing host harness

`LogRing.c` is the serial monitor's capture side: it splits the byte stream
coming out of `/dev/kmsg` into lines, holds them in a fixed ring, and renders
a filtered view of them. It deliberately has no UI and no syscall
dependencies, so it builds with the host compiler and can be exercised
without a QEMU boot.

```sh
./run.sh
```

Covers the cases a boot does not reliably reproduce on demand: a line split
across two reads, a line longer than `LOG_LINE_MAX`, CRLF and padded lines,
control bytes, the ring wrapping past `LOG_MAX_LINES`, case-insensitive
filtering, the newest-N cap on the rendered view, and a render buffer too
small for the match set.

The GUI half (`Main.c`) is not covered here -- it is toolbar wiring on top of
this, and needs the window manager.
