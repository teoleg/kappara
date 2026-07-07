---
name: git-sync
description: Sync the working branch with main, fixing squash-merge drift. Run this at the start of every session before touching any code. Detects already-merged commits and resets the branch to origin/main, then cherry-picks any genuinely unmerged commits on top.
---

# Git sync — fix squash-merge drift

This repo uses squash-merge. Every merged PR creates a new SHA on
main that doesn't match the original commit on the branch, so the
branch always looks "ahead" after a merge. This skill detects and
fixes that.

## Procedure

1. Fetch:
   ```sh
   git fetch origin
   ```

2. Check what's on main that the branch doesn't have:
   ```sh
   git log --oneline HEAD..origin/main
   ```

3. Check what's on the branch that main doesn't have:
   ```sh
   git log --oneline origin/main..HEAD
   ```

4. Decide:
   - If step 3 is **empty** (branch has nothing new): reset to main.
   - If step 3 has commits whose **subject lines match recent merged PRs**
     (compare against `git log --oneline origin/main | head -10`):
     those are squash-merge duplicates — reset to main, they're already in.
   - If step 3 has commits with **genuinely new subject lines** (not in
     main): reset to main, then cherry-pick those commits on top.

5. Reset (always use force-with-lease — safe because we checked above):
   ```sh
   git checkout -B claude/custom-os-build-kUWVJ origin/main
   # if there were genuine unmerged commits: git cherry-pick <sha>...
   git push -u origin claude/custom-os-build-kUWVJ --force-with-lease
   ```

6. Report:
   - How many commits were dropped (already merged).
   - How many were kept (cherry-picked as genuinely new).
   - Confirm: "branch is now N commit(s) ahead of main" (N=0 if nothing new).
