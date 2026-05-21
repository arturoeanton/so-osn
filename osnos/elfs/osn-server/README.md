# osnos servers (ring-3)

Reserved for FASE 10 of the roadmap: move the in-kernel servers
(`shell_server`, `console_server`, `keyboard_server`, `fs_server`)
out of ring 0 and into ring-3 ELFs that talk to the kernel exclusively
through IPC syscalls.

Today these still live under `src/servers/` and run cooperatively
inside kernel mode. The plan is to compile each as a libc-linked ELF
here, get spawned by the kernel at boot like `init`, and reach the
hardware via well-defined syscall + IPC contracts.

Nothing lives here yet. When the migration starts:

  elfs/osn-server/
  ├── shellsrv.c     # ring-3 line editor + command dispatch
  ├── consrv.c       # ring-3 framebuffer writer (via /dev/fb0)
  ├── kbdsrv.c       # ring-3 PS/2 owner (via /dev/input0)
  └── fssrv.c        # ring-3 VFS wrapper (via /sd, /home, ...)
