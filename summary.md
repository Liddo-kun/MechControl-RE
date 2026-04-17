# TweakCC System Prompt Customization — Session Summary

## Goal
Make PowerShell the primary shell tool and Bash a minimal fallback. Remove duplicate instructions between the two. Fix formatting issues.

## Architecture Understanding

### How tweakcc works
1. `data/prompts/prompts-{version}.json` contains "pieces" arrays for each system prompt
2. Pieces define the **search regex** — tweakcc escapes them and joins with `([\w$]+)` capture groups for variables
3. `.tweakcc/system-prompts/*.md` files provide **replacement content**
4. On each run: restore cli.js from backup → apply all prompts in JSON order → write cli.js
5. `.tweakcc/cli.js.backup` stores a clean copy of cli.js (created on first run or version change)

### Critical constraints discovered
- **Multi-piece .md files**: Literal text CAN be changed (the regex searches cli.js for the original text from pieces; the .md provides different replacement text). Variable `${...}` patterns must be in the correct positions relative to the pieces structure. We confirmed this works — the Format-Table change in powershell.md applied successfully.
- **Shared text between Bash and PowerShell**: The PowerShell section appears FIRST in cli.js (~position 8.7M) while the Bash section is later (~9.3M). Prompts are processed in JSON order. Single-piece prompts that match text appearing identically in both sections will match the PowerShell section first.
- **Three dangerous files** that CANNOT be emptied (their single-piece text matches PowerShell first):
  - `bash-git-prefer-new-commits.md` — "Prefer to create a new commit..."
  - `bash-git-avoid-destructive-ops.md` — "Before running destructive operations..."
  - `bash-git-never-skip-hooks.md` — "Never skip hooks..."
  These must keep their original text so the replacement is a no-op.
- **Hard-coded template text**: Headers like "When issuing multiple commands:", "For git commands:", bullet markers (`- `), and some inline content ("To rerun a prior command...", "run_in_background parameter...") are baked into the cli.js template between replaceable pieces. They cannot be controlled through .md files.

## Files Modified

### .tweakcc/system-prompts/ — Content edits (3 files)

**tool-description-bash-overview.md** — Changed body from:
```
Executes a given bash command and returns its output.
```
To:
```
Fallback shell tool. Prefer PowerShell for all terminal operations. All rules from the PowerShell section (dedicated tools, git, sleep, multiple commands) apply here as well.

When to prefer Bash over PowerShell:
- Text-stream piping — Bash pipes raw text, avoiding PowerShell's object-formatting issues (e.g., Select-Object output disappearing)
- Running existing shell scripts (.sh files) or one-liners relying on Unix tools (sed, awk, xargs)
- HEREDOC syntax (<<'EOF') when constructing complex multiline arguments
- Process substitution: diff <(cmd1) <(cmd2)
```

**tool-description-powershell.md** — Two edits:
1. Line 23: Added "Primary tool for all terminal operations. " prefix → **REVERTED** (can re-add, was not the cause of the error)
2. Line 44: Changed `Use Select-Object, Where-Object, ForEach-Object for filtering and transformation` → `Use Format-Table, Format-List, or Out-String to ensure pipeline output is visible.` ✓ WORKING

**tool-description-bash-git-commit-and-pr-creation-instructions.md** — Multiple changes:
- Removed `BASH_TOOL_NAME` from variables list (no longer used)
- `bash commands` → `commands`
- `${BASH_TOOL_NAME} tool` → `PowerShell tool`
- `via the Bash tool` → `via PowerShell`
- `git bash commands` → `git commands`
- `HEREDOC` → `here-string`
- Commit example: HEREDOC syntax → PowerShell here-string (`@'...'@`)
- PR example: HEREDOC syntax → PowerShell here-string (`@'...'@`)

### .tweakcc/system-prompts/ — Emptied files (18 files, header only)
These had body content removed. When tweakcc applies them, the original text in cli.js is replaced with nothing:
- `bash-prefer-dedicated-tools.md`
- `bash-alt-file-search.md`, `bash-alt-content-search.md`, `bash-alt-read-files.md`, `bash-alt-edit-files.md`, `bash-alt-write-files.md`, `bash-alt-communication.md`
- `bash-built-in-tools-note.md`
- `bash-verify-parent-dir.md`, `bash-quote-file-paths.md`, `bash-maintain-cwd.md`, `bash-timeout.md`
- `bash-parallel-commands.md`, `bash-sequential-commands.md`, `bash-semicolon-usage.md`, `bash-no-newlines.md`
- `bash-sleep-run-immediately.md`, `bash-sleep-use-check-commands.md`
- `bash-command-description.md`
- `system-prompt-tool-usage-reserve-bash.md`

### .tweakcc/system-prompts/ — Kept original content (3 files)
These MUST keep original text (their regex matches PowerShell section first):
- `bash-git-prefer-new-commits.md`
- `bash-git-avoid-destructive-ops.md`
- `bash-git-never-skip-hooks.md`

### .tweakcc/system-prompts/ — Already empty (2 files, untouched)
- `bash-sleep-no-polling.md`
- `bash-sleep-keep-short.md`

### .tweakcc/system-prompts/ — Unchanged (1 file)
- `bash-working-directory.md` — Keeps "The working directory persists..."

### Desktop/Instructions.md
Reference document showing desired final state. Bash first (minimal), then Edit/Glob/Grep, then PowerShell (comprehensive), then Read. Order matches cli.js assembly order.

## Current Assembled State

### Bash section — issues remaining
1. **Three bare `-` bullets** near the top (artifacts from emptied pieces where bullet markers are baked into template)
2. **"When issuing multiple commands:" header** with no sub-items (header is hard-coded in template)
3. **"For git commands:" header + 3 git rules** still present (can't empty those .md files)
4. **Inline content** we can't remove: "Instructions" header, "To rerun a prior command...", "run_in_background parameter..."
5. **Some sleep bullets** remain from inline template content

### PowerShell section — issues remaining
1. **"- Avoid unnecessary Start-Sleep commands:"** — has a leading `- ` and wrong indentation. The sleep sub-prompt (`system-prompt-avoiding-unnecessary-sleep-commands-part-of-powershell-tool-description.md`) replaces the original 6-bullet sleep text with its 5-bullet .md content, but indentation differs (2-space in .md vs 4-space in original). Also missing the "If waiting for a background task..." bullet.
2. **"- For git commands:"** section — indented too far (the `${CUSTOM_GIT_NOTES}` conditional in the template adds extra formatting)
3. **No "Primary tool" prefix** — was reverted but CAN be re-added (confirmed the error was from git files, not from this change)

## Next Step: Post-processing Script

Write `postpatch.js` to run AFTER tweakcc. It reads cli.js and does targeted string replacements to fix:

### Bash section fixes:
1. Remove bare `-` bullet artifacts
2. Remove "When issuing multiple commands:" header (and any empty content around it)
3. Remove "For git commands:" header + the 3 git rule lines
4. Clean up any other duplicate/artifact content

### PowerShell section fixes:
1. Fix indentation of "Avoid unnecessary Start-Sleep commands:" section (remove leading `- `, fix indent)
2. Fix indentation of "For git commands:" section
3. Optionally add "Primary tool for all terminal operations." prefix

## Key File Locations
- tweakcc source: `C:\Users\Jon\scratch\tweakcc\`
- tweakcc data: `C:\Users\Jon\scratch\tweakcc\data\prompts\prompts-2.1.92.json`
- System prompts: `C:\Users\Jon\.tweakcc\system-prompts\`
- cli.js backup: `C:\Users\Jon\.tweakcc\cli.js.backup`
- Live cli.js: `C:\Users\Jon\AppData\Roaming\npm\node_modules\@anthropic-ai\claude-code\cli.js`
- Reference doc: `C:\Users\Jon\Desktop\Instructions.md`
- This summary: `C:\Users\Jon\clean\summary.md`
