## What and why
<1–3 sentences. Link the issue or document: Closes #123 · docs/adr/...>

## Type
- [ ] feat  - [ ] fix  - [ ] refactor  - [ ] docs/chore
- [ ] Breaking change (describe the migration)

## Evidence
<output of `./pal check` and `./pal test`, a screenshot or golden diff, or the steps you ran on an hhv2 board>

## Checklist
- [ ] PR title follows Conventional Commits
- [ ] Tests added or updated for the behavior
- [ ] ADR or docs updated if a contract or decision changed
- [ ] Generated files not edited by hand (`sdk/palanu.h`, `docs/API.md`, `api/symbols.csv`, glue)
- [ ] Golden images updated only for scenes that really changed
- [ ] App API: new functions use `PAL_SINCE` and the level goes up; nothing changed or removed
- [ ] Third-party code or assets recorded in docs/SOURCES.md with their license
- [ ] Everything is English (code, identifiers, comments, docs)
- [ ] No secrets (passwords, keys) in the diff

## Built with an AI agent?
- [ ] Yes — I read the whole diff and ran the checks myself
