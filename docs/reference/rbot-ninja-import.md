# Ninja Buildfile Import

rbot can import an existing Ninja build file (`build.ninja`) and convert it
into a rbot `Buildfile`.

There are two modes:

- `-xf` — convert and use temporarily, without saving the generated Buildfile.
- `-xcf` — convert and save the generated Buildfile.

## `rbot -xf`

Use `-xf` when the Ninja build file should only be used for the current
command.

```sh
rbot -xf nbuild/build.ninja
```

rbot converts the Ninja file to a temporary Buildfile, uses it for the
requested operation, and removes the temporary file afterward.

The existing `Buildfile` is not modified.

This is useful when:

- testing an existing Ninja project with rbot;
- comparing rbot against another build system;
- running a one-off build without changing the project;
- using `clean` or another rbot command with an imported Ninja file.

For example:

```sh
rbot -xf nbuild/build.ninja clean
```

The conversion is temporary and the generated Buildfile is removed after
the command finishes.

## `rbot -xcf`

Use `-xcf` when the Ninja build file should become the project's rbot
Buildfile.

```sh
rbot -xcf nbuild/build.ninja
```

The generated configuration is saved as:

```text
Buildfile
```

If a `Buildfile` already exists, rbot asks for confirmation before replacing
it. Declining the confirmation leaves the existing Buildfile unchanged.

After saving, the generated Buildfile can be used normally:

```sh
rbot
```

## Difference

| Command | Convert | Use converted config | Save `Buildfile` |
|---|---:|---:|---:|
| `rbot -xf nbuild/build.ninja` | yes | yes | no |
| `rbot -xcf nbuild/build.ninja` | yes | yes | yes |

Conceptually:

```text
-xf

build.ninja
    |
    v
temporary Buildfile
    |
    v
rbot operation
    |
    v
temporary Buildfile removed
```

```text
-xcf

build.ninja
    |
    v
Buildfile
    |
    v
rbot operation
```

The `-xf` mode is therefore the non-destructive import path, while `-xcf`
is the persistent conversion path.
