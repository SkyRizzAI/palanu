# ADR-0004: MIT license (supersedes ADR-0001)

Date: 13 November 2026. Status: in force.

## Context
ADR-0001 chose Apache-2.0. The project owner decided to use MIT for now.

## Decision
PalanuOS code is licensed under MIT.

## Consequences
- Third-party code keeps its own license and is recorded in `docs/SOURCES.md`.
- Files adapted from Apache-2.0 code keep their original Apache-2.0 header and copyright notice; combining them into an MIT project is allowed as long as the Apache-2.0 terms for those files are met.
- Bundled components (WAMR, Apache-2.0 with the LLVM exception) keep their license files.
- GPLv3 code stays out; only its concepts may be taken.
- MIT does not grant an explicit patent license the way Apache-2.0 does. If that becomes important, this decision is reviewed again.
