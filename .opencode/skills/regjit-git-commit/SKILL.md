---
name: regjit-git-commit
description: Use when preparing to commit code changes, ensuring clear messages and proper git workflow for RegJIT project
---

# RegJIT Git Commit Workflow

## Overview

Write clear, meaningful git commits that document code changes effectively and maintain project history for future developers.

## When to Use

- After completing a feature
- After fixing a bug
- After writing tests
- Before pushing changes
- When preparing pull requests
- During code review

## Commit Message Format

Follow **Conventional Commits** with semantic versioning:

```
<type>(<scope>): <subject>

<body>

<footer>
```

### Type (Required)
Describes the kind of change:

| Type | Purpose | Example |
|------|---------|---------|
| `feat` | New feature | `feat(parser): add quantifier support` |
| `fix` | Bug fix | `fix(anchor): resolve search loop issue` |
| `docs` | Documentation | `docs: update README with examples` |
| `style` | Code formatting | `style: fix indentation in regjit.cpp` |
| `refactor` | Code reorganization | `refactor(codegen): extract IR generation` |
| `perf` | Performance | `perf(matching): optimize character lookup` |
| `test` | Test changes | `test(anchor_quant): add edge cases` |
| `chore` | Build/deps | `chore: update Makefile for LLVM 19` |
| `ci` | CI/CD | `ci: add GitHub Actions workflow` |

### Scope (Recommended)
Component affected:

```
feat(parser): add regex quantifier parsing
fix(anchor): resolve $+ quantifier matching
test(integration): add PCRE compatibility tests
```

### Subject (Required)
- **Imperative mood** ("add" not "added")
- **Lowercase** first letter
- **No period** at end
- **Max 50 characters**
- **Concise and specific**

**Good subjects:**
- `add character class support`
- `resolve anchor quantifier edge case`
- `optimize LLVM IR generation`

**Bad subjects:**
- `Added character class support` (past tense)
- `Fix stuff` (vague)
- `This commit implements new features for regex matching` (too long)

### Body (Recommended for Non-Trivial Changes)

**Format:**
- Blank line after subject
- Wrapped at 72 characters
- Explains "why" not "what"
- Bullet points for multiple changes

**Example:**
```
feat(anchor): implement search loop for quantifier support

The previous implementation only matched at index 0, which failed
for anchor+quantifier patterns like ^*, $+, and \b*. Reference
engines (PCRE, std::regex, RE2) require attempting the match at
every offset in the input string.

This commit:
- Adds outer search loop in Func::CodeGen()
- Generates fresh success/fail blocks per attempt
- Returns 1 on first match, 0 if all fail
- Maintains zero-width assertion semantics

Fixes #42
```

### Footer (Optional)
Metadata and references:

```
Reviewed-by: John Doe <john@example.com>
Fixes #123
Related-to #456
BREAKING CHANGE: ^ now attempts at all offsets
```

## Pre-Commit Checklist

Before committing, verify:

### Code Quality
- [ ] Code compiles: `make clean && make`
- [ ] No compiler warnings
- [ ] No debug code left
- [ ] Follows project style

### Testing
- [ ] All tests pass: `make test_all`
- [ ] New features have tests
- [ ] No test regressions
- [ ] Edge cases covered

### Changes
- [ ] Only intended files modified
- [ ] No sensitive data (passwords, keys)
- [ ] No build artifacts
- [ ] AGENTS.md updated if needed

### Documentation
- [ ] Code comments clear
- [ ] Complex logic explained
- [ ] Public APIs documented
- [ ] Commit message detailed

## Commit Workflow

### 1. Review Changes
```bash
git status                      # See what changed
git diff src/regjit.cpp        # Review specific file
git diff --cached              # Review staged changes
```

### 2. Stage Files
```bash
git add src/regjit.cpp         # Stage specific file
git add tests/test_anchor.cpp  # Stage test file
git add AGENTS.md              # Stage docs
git add -A                     # Stage everything
```

### 3. Verify Staged Changes
```bash
git diff --cached              # Review what's staged
git status                     # Confirm staging
```

### 4. Commit with Message
```bash
# Short message (simple changes)
git commit -m "feat(parser): add quantifier support"

# Long message (complex changes)
git commit                     # Opens editor
# Type full message with body
# Save and exit editor

# Verify commit
git log -1 --stat
```

## Commit Size Guidelines

### Ideal Commit Size
- **Single logical change**
- **Coherent** - related changes only
- **Reviewable** - < 15 minute review
- **Testable** - passes tests independently
- **Atomic** - stands alone

### Good Examples
```
feat(codegen): implement search loop for anchor quantifiers
- Add loop attempting match at every offset
- Generate fresh success/fail blocks per attempt
- Update test_anchor_quant_edge.cpp with edge cases
- Update AGENTS.md with implementation details
- Update Makefile with new test target
```

### Bad Examples
```
# Too large - multiple unrelated features
feat: implement character classes, anchors, quantifiers

# Too small - incomplete feature
feat: add search loop entry point

# Unclear - narrative instead of change
commit: I fixed the anchor matching issue which was preventing
certain patterns from working correctly
```

## Common Commit Patterns

### Feature Implementation
```
feat(charclass): implement character class matching

Add support for [abc], [a-z], and [^abc] patterns with full
range and negation support matching PCRE behavior.

- Add CharClass AST node type
- Implement lexer token types for brackets
- Add parser rules for character classes
- Add comprehensive test coverage in test_charclass.cpp
- Performance verified: < 2% overhead vs std::regex

Implements #34
```

### Bug Fix
```
fix(quantifier): resolve non-greedy matching precedence

Non-greedy quantifiers (*?, +?) were incorrectly matching the
maximum instead of minimum due to inverted greedy flag logic
in Repeat::CodeGen().

Changed: greedy flag now correctly controls matching preference
Test: Passes PCRE compatibility tests in test_quantifier.cpp

Fixes #89
```

### Documentation Update
```
docs: clarify anchor quantifier search mode requirement

Updated AGENTS.md with comprehensive explanation of:
- Why search loop is necessary for compatibility
- Code implementation example
- Test coverage overview
- Maintainer notes for future changes
```

### Refactoring
```
refactor(parser): extract token handling into utilities

No behavior change. Extract repetitive token parsing into
reusable utility functions for better maintainability and
reduced code duplication.

Functions extracted:
- parseCharacter() for single char matching
- parseEscape() for escape sequences
- parseMetaChar() for special characters

Test: All existing tests still pass
```

## Fixing Mistakes

### Amend Last Commit (Not Yet Pushed)
```bash
# Fix code, then:
git add .
git commit --amend --no-edit     # Keep original message
git commit --amend               # Edit message
git log -1 --stat                # Verify
```

**⚠️ Only if not pushed to remote!**

### Revert Commit
```bash
git revert <commit-hash>  # Creates new commit undoing changes
git log -1                # Verify revert commit created
```

## Viewing Commit History

```bash
# One-line format
git log --oneline

# Show full history
git log --graph --oneline

# Show with diffs
git log -p

# Filter by author
git log --author="name"

# Filter by date
git log --since="2 weeks ago"

# Search commit messages
git log --grep="feat"

# View specific commit
git show <commit-hash>
```

## Best Practices

- [ ] Commit frequently (don't wait for massive commits)
- [ ] Write descriptive messages (future you will thank you)
- [ ] Reference issues (`Fixes #123`)
- [ ] Keep commits focused and atomic
- [ ] Test before committing
- [ ] Review diffs before staging
- [ ] Don't commit debug code
- [ ] Keep history clean and linear

## RegJIT Commit Examples

### Recent Good Commits
```
feat: implement anchor/quantifier search mode
- Implement search loop in Func::CodeGen()
- Support ^*, $+, \b* patterns
- PCRE/RE2 compatible
- Add comprehensive edge case tests

fix: resolve anchor quantifier behavior
- Fix zero-width assertion handling
- Verify search loop covers all offsets
- All PCRE compatibility tests pass

test(anchor_quant): add edge case coverage
- Test ^*, ^+, ^{n} patterns
- Test $*, $+, ${n} patterns
- Test \b*, \b+ boundary quantifiers
- Verify empty string handling
```

## Quick Reference

| Task | Command |
|------|---------|
| View status | `git status` |
| See changes | `git diff` |
| Stage file | `git add file.cpp` |
| View staged | `git diff --cached` |
| Commit | `git commit -m "message"` |
| View log | `git log --oneline` |
| View commit | `git show <hash>` |
| Amend last | `git commit --amend` |
| Revert commit | `git revert <hash>` |
