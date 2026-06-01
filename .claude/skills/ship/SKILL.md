---
name: ship
description: Full pre-commit gate for kappara. Use when the user says "ship it", "ready to commit", "prep for push", "looks good to commit?", "land this". Runs build → boot test → doc-sync → svr4-check; if all pass, proposes a commit message and waits for approval before committing and pushing to the development branch.
---

# Ship gate

A sequenced gate. Each step's failure is terminal — stop and report, don't auto-fix unless the fix is trivially mechanical (e.g. obvious typo in a doc).

## Sequence

1. **Diff sanity.**
   `git status` + `git diff --stat`. If the working tree is empty, stop with "nothing to ship".

2. **Build clean.**
   Invoke the `build` skill (default ARCH=aarch64). If it fails, stop and surface the error verbatim.

3. **Boot test.**
   Invoke the `test` skill with no args. Must reach `kappara:/# ` prompt with no panic, no character interleaving, all expected idle threads visible. If it fails, stop.

4. **Doc-sync audit.**
   Invoke the `doc-sync` skill. Any MISSING entries → stop. Tell the user what to add; do not edit docs autonomously unless the missing edit is mechanical (e.g. one-line entry in a list with an obvious slot).

5. **SVR4 + bug-class review.**
   Invoke the `svr4-check` skill. BLOCKING verdict → stop. `OK with comments` → continue but surface the comments.

6. **Compose commit message.**
   - **Subject:** imperative, ≤60 chars, no trailing period.
   - **Body:** explains **why**, not what. Reference prior commits by short hash when relevant (this codebase leans on history a lot — `aa8759f`, `0929814`, `983e1c2` are recurring touchstones).
   - **Do not include:** model-id / co-author tags, "🤖 generated" lines, or any boilerplate.

7. **Present.**
   Show:
   - One-line summary of what changed
   - Files touched (count + categories: kernel / arch / docs / user)
   - Proposed commit message in a fenced block
   - The exact `git commit` and `git push -u origin claude/custom-os-build-kUWVJ` commands you would run

8. **Wait for approval** (a yes / "go" / "land it" from the user). Do not commit autonomously, even if every gate passed.

## On approval

- `git add` only files in the diff (no `-A`, no `.`).
- Commit via heredoc (preserves multi-line body formatting).
- `git push -u origin claude/custom-os-build-kUWVJ`. If push fails with a network error, retry up to 4 times with 2s/4s/8s/16s backoff.
- Report the push result and stop.

## On any gate failure

- Surface the exact failure with file:line where applicable.
- Do not loop / retry / paper over.
- If the failure is a doc-sync miss and the missing edit is a one-line entry in an obvious slot, you may make the edit and re-run from step 4. Anything else → ask.
