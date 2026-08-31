# ImplusOS Userland-Common

Shared userland plumbing for ImplusOS: the init process (`Userland.c`,
`_start` entry point), the userland linker script (`Userland.ld`), and
`AppCommon.mk`, the shared Makefile fragment every app and service
includes to pull in [API](https://github.com/ImplusOS/API),
[I_libc](https://github.com/ImplusOS/I_libc), and
[Library](https://github.com/ImplusOS/Library).

This repository is a component of **[ImplusOS](https://github.com/ImplusOS)**,
a hobby operating system with a monolithic kernel, loadable driver modules,
a minimal freestanding C library, and a small graphical userland. It is not
meant to be built in isolation -- it is consumed as a checkout alongside
ImplusOS's other component repositories (see `Docs` for the full
architecture and `ImplusOS/Makefile` for how the pieces are wired together).

## Layout

```
Userland-Common/
├── Source/    All source for this component, structure preserved from ImplusOS
└── README.md  This file
```

## Build

No standalone Makefile: `AppCommon.mk` is included directly by each
app/service Makefile (e.g.
[com.ImplusOS.windowmanager](https://github.com/ImplusOS/com.ImplusOS.windowmanager)).

## License

MIT, matching the parent [ImplusOS](https://github.com/ImplusOS/ImplusOS) project.
