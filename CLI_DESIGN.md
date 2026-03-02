# CLI Design

## Model

- `store/` holds immutable package objects
- profiles are composed views of selected store objects
- activation switches the live system to a chosen profile or active set
- downloads extract package artifacts into `store/`
- no command performs a traditional direct install into `/usr`

## Package Artifact

- external extension: `.pkg`
- internal format: `tar` compressed with `zstd`
- one artifact per store object
- naming:
  - runtime: `<name>-<version>-<hash>.pkg`
  - development split: `<name>-devel-<version>-<hash>.pkg`

## Command Families

### Build

- `pkg build <port>`
- `pkg build --group <name>`
- `pkg build --activate <port-or-group>`

Behavior:
- builds source into `store/`
- `--activate` also updates the active profile

### Package

- `pkg package <port>`
- `pkg package --group <name>`
- `pkg package --upload <port-or-group>`

Behavior:
- packages existing store objects into `.pkg` artifacts
- `--upload` publishes those artifacts to the remote package repository

### Download

- `pkg download <port>`
- `pkg download --group <name>`
- `pkg download --activate <port-or-group>`

Behavior:
- downloads `.pkg` artifacts
- verifies them
- extracts them into `store/`
- `--activate` also updates the active profile

### Activate

- `pkg activate <port>`
- `pkg activate --group <name>`
- `pkg activate --profile <name-or-path>`

Behavior:
- recomputes the active profile from the requested selection
- projects the active profile into the live root

### Deactivate

- `pkg deactivate <port>`
- `pkg deactivate --group <name>`
- `pkg deactivate --profile <name>`

Behavior:
- removes the selection from the active set
- recomputes the resulting profile
- reprojects the live root

## State Model

- `base` is the runtime floor
- higher layers like `development` are additive
- deactivation removes a layer or port from the active set
- the active system is always the recomputed result, not manual unlinking

## Split Policy

- runtime packages contain executables, shared libraries, runtime data
- `-devel` packages contain headers, static libraries, unversioned linker symlinks, pkg-config files, and build metadata

