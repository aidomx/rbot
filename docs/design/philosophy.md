# Design Philosophy

## A simple builder for you

The central idea is deliberately simple:

> **A simple builder for you.**

This does not mean rbot has few features. It means the user should not have to describe implementation details that a build tool can determine reliably itself.

The desired direction is therefore:

```text
more capability inside rbot
less boilerplate in Buildfile
```

## Convention first

Most ordinary C projects share the same broad rules:

- source files
- include directories
- a compiler
- compiler flags
- dependencies
- object files
- a binary and/or library
- a build directory

Because these rules repeat, a standard project should require almost no configuration.

The proposed minimal initialization is:

```text
use project
```

and for a workspace:

```text
use workspace
```

These two forms are design direction discussed during development; they represent the desired simple user-facing surface rather than a claim that every detail is already implemented in v0.2.0.

## Complexity belongs in the tool

A useful rule for rbot is:

> If the same configuration appears in almost every project, it is probably a candidate for a default.

Configuration should primarily express deviations from convention.

For example, an explicit project might eventually look like:

```text
use project

compiler = clang
flags = Wall, Wextra
headers = include, vendor/foo
sources = src, generated
```

rather than forcing every normal project to repeat all of those fields.

## Performance without benchmark mythology

rbot should not define its identity as "faster than Ninja". Build performance naturally fluctuates with OS, filesystem, CPU scheduling, cache state, compiler, and environment.

The more useful objective is:

- low overhead,
- correct incremental behavior,
- predictable results,
- reasonable cold builds,
- and strong behavior across environments.

Sometimes Ninja should win. Sometimes rbot should win. The important thing is that rbot remains competitive without becoming fragile.

## Tolerant environments

A build tool is often used in environments that are not ideal: Termux, proot-distro, containers, old systems, different compilers, or Windows. Robustness across these environments can be more valuable than a tiny benchmark advantage on one machine.

A useful mental model is:

```text
super fast  -> can be beaten by another implementation
hardy       -> requires many different failure cases to be handled well
```

The design target is therefore **fast enough and hard to break**.
