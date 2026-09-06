# Host harness for the Wayland compositor

`Compositor.c` and `Wayland.c` talk to the outside world only through the
ImplusOS syscall wrappers, so both files also compile on a Linux build host
once something supplies that surface. `HostShim.c` does: AF_UNIX syscalls
become real sockets, shared-memory handles become `memfd_create` fds, and the
output becomes a plain 1280x800 buffer dumped as PPM.

That is enough to run the **unmodified Debian `gtk3-demo` the image ships**
against the real compositor code, with the real `libwayland-client` doing the
marshalling -- which is where wire-protocol mistakes (a wrong opcode, a
mis-sized argument list, a missing `wl_pointer.frame`) surface for the price
of a `gcc` run rather than a QEMU boot.

    make -C ../../../.. linux_runtime_stage    # once, to stage the Debian tree
    ./run.sh                                   # gtk3-demo for 8 s
    ./run.sh "$PWD/../../../../Build/x86_64/LinuxRuntime/stage/usr/bin/gtk3-widget-factory" 10
    python3 ppm2png.py .work/run/frame020.ppm /tmp/frame.png

`WLC_TRACE=1 ./run.sh` adds a line per request (`-DWLC_PROTOCOL_TRACE`).

Scripted input, via `WLC_INPUT=<file>`, one event per line, delivered once
that many frames have been presented:

    8  m -290 -243 0     # pointer delta, button mask
    10 m 0 0 1           # press button 1
    12 m 0 0 0           # release
    20 k 57424 1 0       # set-1 scancode 0xE050 (Down), pressed

What this harness does **not** cover: the hosted (window-manager) backend, the
panel takeover when the WM exits, and anything that depends on the ImplusOS
kernel's own AF_UNIX/SCM_RIGHTS and shared-memory implementations. Those still
need a QEMU boot.
