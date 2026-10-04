# Errors and Interrupts

## SIGINT

rbot handles Ctrl+C and forwards interruption to active child processes. This is important for compilers running in parallel and for environments where process groups behave differently from a conventional shell.

A cancelled build must not be recorded as a successful build state.

## Error output

Progress/error policy can be configured. A useful development configuration is:

```text
progress.error = always
```

The exact terminal formatting is intentionally an implementation detail; scripts should rely on process exit status rather than presentation text.
