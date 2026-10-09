# Host foundation builds

Requires CMake >= 3.20, Ninja, Python >= 3.12, and a C/C++ compiler with C++20
support (GNU or Clang for the warning policy and AddressSanitizer).

Supply the pinned GoogleTest v1.15.2 archive offline via
`AA_GOOGLETEST_ARCHIVE=/path/to/googletest-v1.15.2.tar.gz` in the environment,
or place it at `.local/dependencies/googletest-v1.15.2.tar.gz` (gitignored).
The archive hashlock and original-source provenance checks always run before
staging. Nothing is downloaded. A cache override in a gitignored
`CMakeUserPresets.json` may also set `AA_GOOGLETEST_ARCHIVE`; an existing cache
value takes precedence over the environment.

For each of `host-dev`, `host-release`, and `host-asan`, run:

```sh
set -euo pipefail
cmake --preset host-dev
cmake --build --preset host-dev
ctest --preset host-dev --output-on-failure
```

Each preset has a separate ignored `build/<preset>/` directory and exports
`compile_commands.json`. The project-owned smoke target uses strict C++20,
GNU/Clang warnings as errors, an explicit source inventory, and GoogleTest
assertions that remain active in Release. ASAN adds compile and link
instrumentation only to project-owned code; vendor standards are untouched.
This builds GoogleTest/GoogleMock and the host smoke test, not the full AASDK.
SDK ABI qualification and missing development libraries belong to task 7.

Use the installed tools on owned sources:

```sh
set -euo pipefail
clang-format --dry-run --Werror tests/host/smoke.cpp
clang-tidy -p build/host-dev tests/host/smoke.cpp
clangd --check=tests/host/smoke.cpp --compile-commands-dir=build/host-dev
```

`.clangd` directs editor diagnostics to the development compilation database.
Clangd 18's `--check` can report an ExtractFunction refactoring self-test failure
on GoogleTest macros even with a clean AST. For that tool version, add
`--tweaks=DefineInline` to check parsing/diagnostics without that broken
refactoring self-test; this does not suppress compiler diagnostics.
