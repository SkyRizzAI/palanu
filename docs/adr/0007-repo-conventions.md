# ADR-0007: Follow the repository conventions, adjusted for a C project

Status: accepted.

## Context
The project's repository conventions (v1, 25 Oct 2026) are the default standard for a new repo, written for a TypeScript monorepo. PalanuOS is a C project
with Bun tooling in `forge/`. The conventions allow deviations when the reason is written down.

## Decision
Followed: `AGENTS.md` of about 100 lines with `CLAUDE.md` containing `@AGENTS.md`; `docs/ARCHITECTURE.md`; ADRs; Conventional Commits; CI that
runs the same commands as local; PR and issue templates; CODEOWNERS; Dependabot; secret scanning; `CONTRIBUTING.md`,
`CODE_OF_CONDUCT.md`, `SECURITY.md`, `LICENSE` and `NOTICE` for an open repo; contract first (API in `api/palanu/*.h`), output generated.

Adjusted:
- ADRs stay in `docs/adr/` (not `docs/decisions/`): already in use and referenced in many places.
- The single gate command is `./pal check` followed by `./pal test` (not `bun run check`): a C toolchain, not turbo.
- The file size limit follows `docs/CODING-RULES.md` (about 800, hard 1000), checked by `./pal check`.
- No Changesets: the version is in `VERSION` and `CHANGELOG.md`, bumped by `./pal release`; there is no release bot yet.
- Branches: model B (trunk + PR) once the repo is public; a single `main`, no `dev`.
- Not applicable: Turborepo, oRPC/Drizzle, EAS, Docker server, `modules/` vertical slices.
