# Field Notes from rbot Development

These notes summarize observed behavior during development and testing. They are not normative benchmark guarantees.

## Native vs proot

Build performance can fluctuate between native Termux, Debian/proot-distro, and conventional desktop Linux. Filesystem and process overhead can change the relative ordering between rbot and Ninja.

A practical observation from a larger real project was a cold build of roughly:

```text
208 C source files
150+ headers
about 3 minutes
```

in Debian/proot-distro, with repeated runs reported as relatively stable.

This is valuable evidence of robustness, but it should not be presented as a universal speed claim.

## Relative performance

Across repeated local measurements, Ninja sometimes leads and rbot sometimes leads. The important result is that rbot is often in the same performance class for the tested workloads while providing its own content-aware and configuration model.

## Benchmark honesty

A build tool should be judged by the total engineering experience:

- correctness
- incremental behavior
- portability
- reproducibility
- diagnostics
- configuration burden
- build time

Raw elapsed time is one measurement, not the entire product.
