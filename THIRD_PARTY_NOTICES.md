# Third-Party Notices

This project uses the following third-party components.

## daemon/third_party (downloaded, not committed)

These files are fetched by `scripts/fetch_deps.sh` and are pinned to specific upstream tags/versions:

- **miniaudio** (`miniaudio.h`) by David Reid (mackron)
  - Used for audio output
  - License: MIT-0 / public domain style (see upstream project)
- **cpp-httplib** (`httplib.h`) by Yuji Hirose
  - Used for HTTP server/API
  - License: MIT (see upstream project)

## System / Distro Dependencies

- **libbpf**
  - Used for eBPF userspace loading and skeleton support
  - License: LGPL-2.1/BSD-2-Clause (varies by component; see libbpf)
- **bpftool**, **clang/llvm**
  - Used at build time to generate `vmlinux.h` and compile the BPF object

## UI Dependencies

The UI is built with React + Vite and includes dependencies listed in `ui/package.json`.
Licenses are provided by their respective projects (see `ui/node_modules/*/LICENSE*` after install).

