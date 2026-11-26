# Local development

Build and test on x86-64 Linux or WSL. Use a Linux filesystem for the compiler and boot tests. Run `make check` after code changes and `make check-memory` for changes to host C or C++ code.

Build the desktop client on x64 Windows with `retrieval/ui/build.ps1`. Keep verification local.

## Code Review Rules

Report each defect once. Identify the affected code, a failing input or build configuration, and the expected result. Omit status summaries and style-only comments.
