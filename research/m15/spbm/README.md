# GB10 SPBM measurement reader

This directory contains preliminary R15 telemetry work. It does not issue an
R15 PASS. The complete performance harness and qualification remain required.

## Reader boundary

`aien_spbm_readonly.c` is an out-of-tree Linux platform driver for this
machine's ACPI `NVDA8800` device. Before mapping anything, it checks the
firmware's `_DSM` resource name and all fifteen permitted register names and
offsets against `contract.h`. It requires exactly the expected 4 KiB SPBM
resource and reserves it through `devm_ioremap_resource`. A changed firmware
layout fails the probe. There is no fallback address, module parameter,
power-cap control, MMIO write, or access to CLEAR_OVERFLOW registers.

The ten hwmon measurement attributes and five raw overflow attributes are
read-only. An energy read checks its overflow indicator before and after
reading the counter and returns `EOVERFLOW` if either is nonzero. Firmware
rollover semantics are not experimentally established. The sampler rejects a
backward counter, any overflow, insufficient counter headroom for the window,
and malformed or missing samples; it does not guess how to unwrap them.

Register locations were independently checked against this machine's
disassembled DSDT (SHA-256
`2871cbcb8992f7bd3915d6f19e7cfab577e068dcad58b6f00317a9f2d4e1b28e`).
The ACPI discovery protocol and milli-unit convention are also described by
[spark_hwmon](https://github.com/antheas/spark_hwmon/blob/352c76e538cadece0e4887a153fef95f8f5ea1d4/spbm.c),
licensed GPL-2.0. This reader follows that protocol but implements a narrower
read-only contract. The firmware table supplies names and offsets, not a
published accuracy specification.

## Units and limits

Raw energy is interpreted as 32-bit millijoules, exposed as microjoules. Raw
power is interpreted as milliwatts, exposed as microwatts. The representational
energy step is 1 mJ; that is not a claim of 1 mJ measurement accuracy.

Energy channels are package, CPU-E, CPU-P, GPC and GPM. GPC stayed at zero in
every collected window and is explicitly labelled `gpc_unverified`. It must
not be used as evidence of zero GPU energy. GPM is the functioning graphics
counter. The separately sampled NVML graphics counter covers a different
measurement boundary; equality with GPM is not assumed.

The package energy rate exceeded the integrated instantaneous package power
by 0.27 to 0.52 W in the completed vector-load collection. The source of that
discrepancy has not been established. Do not silently subtract it or call it
calibrated accuracy. Neither source was compared with an external meter.
These are on-device package measurements, not wall-outlet measurements.
They cannot confirm or rule out a short electrical disturbance at the outlet.

CPU sensor reads and NVML calls have separate timestamp intervals. NVML may
block for several milliseconds; that delay must not be assigned to the CPU
counter observation. All raw read intervals are retained.

## Preliminary collections

- `runs/preflight-20260928-01`: initial scalar load, completed. Its older
  timestamp format grouped NVML with CPU sensor reads; exploratory only.
- `runs/preflight-20260928-02`: vector load, aborted when that grouped read
  span exceeded 10 ms. Partial records are retained; no successful status.
- `runs/preflight-20260928-03`: corrected independent timestamps; three
  idle, three CPU-P and three CPU-E windows, 101 samples per 10-second
  window. It completed. `summary.json` is exactly reproduced by `reduce`.

In collection 03, package energy rates were 19.65 to 19.85 W at idle and
22.79 to 22.96 W under the CPU-P load. CPU-P's own rate rose from 1.87 to 2.07 W
to 6.78 to 6.86 W. CPU-E's rate rose from 0.28 to 0.33 W at idle to 2.02 to 2.22 W
under its load. All overflow indicators were zero. Each worker's CPU time and
verified core affinity are recorded. Other machine activity was present and
recorded; these runs are not a quiet-machine R15 qualification.

Checks completed: signed reader loaded with Secure Boot enabled; firmware
contract accepted; malformed register contracts rejected under address and
undefined-behavior sanitizers; measurement attributes read-only; collection
checksums verified; summary reproduced byte-for-byte; energy and integrated
power calculations independently recomputed; damaged records with overflow,
counter decrease, missing sample, and incorrect sequence rejected.

After the operator reported a power surge, the load processes were confirmed
stopped and the telemetry module was unloaded. The code changed no power
limit or CPU governor. The operator has since authorized continuing work;
give a clear heads-up before further load-generating tests.

## Reproduction

`make` builds the module against the running kernel; `make tools check`
builds the small C sampler, bounded CPU load, reducer and contract checks.
The module must be signed with the already enrolled owner key before manual
loading. No service, boot autoload configuration, or power-control setting is
installed. The private key remains outside this repository.

`bash collect.sh NEW-DIRECTORY` deliberately drives ten CPU cores for
14 seconds per load period, six periods total. It increases CPU activity and
can increase power draw. The sampler records 10 seconds within each period.
The collector refuses an existing directory and stops its load child on exit.
The module must already be loaded; the collector does not load it.

`./reduce RUN-DIRECTORY` reads only saved files and reproduces the V2 summary.
`sha256sum -c SHA256SUMS` inside a completed run verifies its recorded files.
The source/binary checksum list is checked from this directory. Earlier
exploratory runs intentionally bind earlier source versions.
