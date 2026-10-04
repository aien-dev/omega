# License note for research/m15/spbm

Omega is licensed AGPL-3.0-or-later (see the LICENSE at the repository root, decision of Drake Stapleton, 2026-10-04).

The six source files in this folder (`aien_spbm_readonly.c`, `load.c`, `reduce.c`, `sample.c`, `contract.h`, `contract_test.c`) carry `SPDX-License-Identifier: GPL-2.0-only` and stay GPL-2.0-only. `aien_spbm_readonly.c` is a Linux kernel module, which is why it uses the kernel's license.

Build status, checked on 2026-10-04 by searching the top-level `Makefile` and `mk/*.mk`: nothing there refers to `research/` or `spbm`, and the default target `all` builds only `$(TARGET)`. This folder has its own `Makefile` and is built only by hand (`make -C research/m15/spbm`). The omega build does not compile or link these files.

At run time, some tools under `tools/` (`r15_machine_state.sh`, `r15_qualify.sh`) only read the loaded module's energy counter from the system's sysfs files. They do not include or link this code.

These files are research-only and must not be linked into a shipped omega binary.
