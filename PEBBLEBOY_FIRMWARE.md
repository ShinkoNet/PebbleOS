# Pebbleboy firmware for Pebble Time 2

This fork publishes an optional PebbleOS build for Pebbleboy on Obelix
(Pebble Time 2). It tracks stable upstream PebbleOS tags and carries two
patch sets:

- keep speaker DMA refills ahead of CPU-heavy foreground apps;
- expose a UUID-scoped app blob API for verified payloads up to 8 MiB in the
  shared PFS flash filesystem.

Pebbleboy uses the blob API to install the configured cartridge once over
AppMessage and then read it locally while playing. A blob is invisible until
its full payload passes CRC32 verification, survives app upgrades, and is
deleted when its owning app is removed. The API is generic rather than tied to
ROMs, but each app UUID has only one blob and filesystem free space remains
shared with other watch data.

Repeated reads reuse one validated PFS session until the app exits or mutates
its blob. Cartridge cache misses therefore seek directly to their payload
instead of reopening the file and validating both headers for every line.

Because earlier Pebbles expose smaller app regions, Pebbleboy only supports
Pebble Time 2 and Pebble 2 Duo.

The firmware is still PebbleOS and remains licensed under Apache-2.0. The
changes are maintained as ordinary commits on top of the upstream release so
they can be reviewed or proposed upstream independently.

## Install

Download the merged `.pbz` for the hardware revision printed by your watch:

- `normal_obelix_pvt_*.pbz` for production Pebble Time 2 watches; or
- `normal_obelix_dvt_*.pbz` only for DVT development hardware.

The merged PBZ contains both firmware slots. Use the normal Pebble sideload
flow so the installer can select the slot opposite the currently running
system. Do not install a DVT image on PVT hardware or the reverse.

Custom firmware is inherently riskier than an app sideload. Charge the watch,
keep it connected throughout the update, retain a known-good upstream PBZ, and
read the release notes before installing.

## Automated releases

`.github/workflows/pebbleboy-obelix-release.yml` checks once per day for the
latest stable upstream tag. When a new tag appears it:

1. rebases this fork's commits onto that exact tag;
2. tags the resulting source as `vX.Y.Z-pebbleboyN`;
3. builds Obelix DVT and PVT for both firmware slots;
4. merges each slot pair into one sideloadable PBZ; and
5. publishes a prerelease with checksums, ELF files, and log dictionaries.

The workflow can also be dispatched manually for a specific upstream tag. If
an upstream change conflicts with the patches, the rebase fails instead of
silently publishing an unpatched or partially patched image.
