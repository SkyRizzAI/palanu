# ADR-0001: Apache-2.0 license (superseded by ADR-0004: MIT)

Status: accepted (13 November 2026)

## Context
PalanuOS is written from scratch. Some code and concepts come from other projects, under several licenses: code owned by the project owner (GPLv3), Apache-2.0 code, and GPLv3 code.

## Decision
The project is licensed under Apache-2.0.

## Reasons
- Code owned by the project owner can be relicensed.
- Apache-2.0 code may be adapted while keeping its copyright notices.
- Apache-2.0 grants patent rights and does not force third-party apps to be open.

## Consequences
- GPLv3 code (including GPLv3 assets) must not come in. Concepts are allowed; the code is written ourselves.
- If a GPLv3 part proves clearly the best, the license is reviewed openly through a new ADR, not quietly violated.
- The project owner needs to confirm this decision; the status "accepted" holds until there is an objection.
