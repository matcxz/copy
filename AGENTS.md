# Project overview

`copy` is an independent, lightweight alternative to `cp`, implemented in C.
It aims to remain a small, separately distributed project rather than a
complete reimplementation of GNU coreutils. It supports Linux only and has no
external runtime dependencies.

# Layout

- `src/common.h`: feature-test macros and shared system includes.
- `src/options.h`: command-line `Options` structure.
- `src/main.c`: option parsing, usage, top-level copy flow, and program entry.
- `src/util.c`, `src/util.h`: errors, dry-run output, path helpers, and file identity.
- `src/progress.c`, `src/progress.h`: progress state, display, and tree scanning.
- `src/verify.c`, `src/verify.h`: file comparison and disk verification preparation.
- `src/transfer.c`, `src/transfer.h`: data transfer, sparse files, and special destinations.
- `src/resume.c`, `src/resume.h`: resumable temporary files and publication.
- `src/file.c`, `src/file.h`: regular-file metadata and symbolic-link copying.
- `src/tree.c`, `src/tree.h`: recursive traversal and entry dispatch.
- `tests/`: Python `unittest` suite for copy and resume behavior.

# Build and test

- `make` builds the `copy` executable.
- `make test` runs the Python `unittest` suite in `tests/`.
- `make check` runs the same suite with AddressSanitizer and UndefinedBehaviorSanitizer.
- Tests use the `COPY_BIN` environment variable to select the executable.

# Coding rules

- Use C11 and compile cleanly with `-Wall -Wextra -Wpedantic -Werror`.
- Do not add comments to code or use em dashes in text or code.
- Handle `EINTR` and partial reads and writes.
- Check every system call result and report failures explicitly.
- Do not follow symbolic links unless intended; use `O_NOFOLLOW` where appropriate.
- Prefer `fstat` on an open descriptor over `stat` on a path.

# Behavior contract

Match GNU `cp` except for differences documented in `README.md`.

- New files and directories use the source mode filtered by the process umask.
- Setuid, setgid, and sticky bits are preserved only with `-p`.
- A new destination must never have permissions broader than the source mode.
- Existing destination modes are retained unless `-p` requests preservation.
- Non-regular destinations, including `/dev/null`, pipes, and FIFOs, must work.
- Recursive copying into the source directory or its descendants is refused.
- Sparse files remain sparse, and `-c` verifies data after syncing it to disk.

# Testing rules

- Every bug fix and feature must have a corresponding test.
- Compare edge-case behavior with the system `cp` where applicable.
- Permission tests must cover umasks `022` and `077`.
- Keep tests deterministic and use temporary directories for filesystem cases.

# Commit rules

- Make one focused change per commit.
- Use imperative commit subjects.
- Never mix refactoring with behavior changes.
- Run `make test` and `make check` successfully before each commit.

# Out of scope

Do not add features unless explicitly requested.
