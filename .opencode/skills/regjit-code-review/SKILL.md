---
name: regjit-code-review
description: Use when reviewing RegJIT code changes for quality, correctness, and PCRE/RE2 compatibility
---

# RegJIT Code Review

## Overview

Systematically review code changes in RegJIT for quality, correctness, and adherence to regex engine compatibility standards (PCRE, std::regex, RE2).

## When to Use

- After writing significant code changes
- Before committing new features or fixes
- When reviewing anchor/quantifier behavior changes
- When modifying LLVM IR generation
- Before merging branches

## Review Checklist

### 1. Scope & Context (5 min)
- [ ] Identify all modified files
- [ ] Understand change purpose (feature/fix/refactor)
- [ ] Check alignment with commit message
- [ ] Verify no unintended files modified

### 2. Functionality (15 min)
- [ ] Logic is correct and matches intent
- [ ] Edge cases handled (empty input, single char, boundaries)
- [ ] Search loop working correctly for anchors with quantifiers
- [ ] PCRE/RE2 behavior verified

**Key checks for RegJIT:**
- Anchor semantics: `^` and `$` match at correct positions only
- Zero-width assertions: `^`, `$`, `\b`, `\B` behave as zero-width
- Quantifier precedence: applied to correct elements
- Search loop: attempts match at every offset (0 to strlen inclusive)

### 3. Code Quality (10 min)
- [ ] Consistent style (naming, indentation)
- [ ] Comments explain "why" not just "what"
- [ ] No magic numbers without explanation
- [ ] LLVM IR generation follows patterns

**RegJIT specific:**
- BasicBlock naming is descriptive
- PHI node usage correct for loops
- No dead code blocks
- SSA form maintained

### 4. Architecture (10 min)
- [ ] Follows AST node pattern
- [ ] Proper separation between Lexer/Parser/CodeGen
- [ ] LLVM integration correct
- [ ] No circular dependencies

### 5. Testing (5 min)
- [ ] Tests added for new functionality
- [ ] Existing tests still pass
- [ ] Edge cases covered
- [ ] PCRE/RE2 compatibility verified

### 6. Documentation (5 min)
- [ ] Code comments clear and accurate
- [ ] AGENTS.md updated if adding features
- [ ] Complex algorithms explained
- [ ] Search loop note present if modifying Func::CodeGen()

## Common RegJIT Issues to Watch

### LLVM-Specific
- [ ] Using wrong BasicBlock for branching
- [ ] All code paths lead to return statement
- [ ] Memory leaks in LLVM objects (use smart pointers)
- [ ] SSA form violations (redefining values)

### Anchor/Quantifier
- [ ] Search loop preserved (critical for compatibility)
- [ ] Zero-width detection working (isZeroWidth())
- [ ] Anchor attempts at correct offsets
- [ ] Quantifier properly applied to expressions

### C++ Quality
- [ ] No use-after-free bugs
- [ ] Proper RAII usage
- [ ] Smart pointers for memory safety
- [ ] Const correctness maintained

## Code Review Process

1. **Read the diff** - understand changes at high level
2. **Check logic** - trace through critical paths
3. **Verify tests** - ensure coverage is adequate
4. **Look for issues** - use common issues checklist
5. **Document findings** - note specific file:line references
6. **Provide feedback** - be specific and actionable

## Example Review

```
## Code Review: anchor quantifier search loop

### Files
✅ src/regjit.cpp - Core matcher with search loop
✅ tests/test_anchor_quant_edge.cpp - Comprehensive test coverage
✅ AGENTS.md - Documentation updated
✅ Makefile - Test target added

### Functionality
✅ Search loop attempts match at every offset
✅ Zero-width anchor semantics preserved
✅ PCRE/RE2 compatible for ^*, $+, \b*, etc
✅ Edge cases handled (empty string, end of string)

### Code Quality
✅ Variable names descriptive (LoopCheckBB, TrySuccess)
✅ Comments explain search loop purpose
✅ IR structure clean and correct
✅ No compiler warnings

### Architecture
✅ Follows existing patterns
✅ LLVM integration proper
✅ No regressions in other features

### Testing
✅ 15 edge case tests added
✅ All existing tests pass
✅ Verified against PCRE behavior

### Result
✅ APPROVED - High quality implementation with excellent compatibility
```

## Red Flags

| Flag | Severity | Action |
|------|----------|--------|
| Search loop removed/modified | 🔴 Critical | Ask why, verify PCRE compat |
| Anchor behavior changed | 🔴 Critical | Must verify with PCRE/RE2 |
| New zero-width code | 🟡 High | Check isZeroWidth() logic |
| LLVM IR dead code | 🟡 High | Remove or explain |
| Quantifier modification | 🟡 High | Verify greedy/non-greedy |
| Missing test for feature | 🟠 Medium | Request tests added |
| AGENTS.md not updated | 🟠 Medium | Minor, can update later |
| Code comments unclear | 🟢 Low | Ask for clarification |

## Questions to Ask

1. **Why did you choose this approach?** - Understand design intent
2. **What edge cases could break this?** - Uncover missing handling
3. **How does this compare to PCRE?** - Verify compatibility
4. **What tests verify this works?** - Ensure coverage
5. **Will this impact performance?** - Check optimization
6. **Is this the simplest solution?** - Prefer clarity over cleverness

## Success Criteria

- [ ] Code compiles without warnings
- [ ] All tests pass (make test_all)
- [ ] No memory leaks
- [ ] PCRE/RE2 compatible
- [ ] Clear comments and documentation
- [ ] Follows project patterns
- [ ] Ready to commit

## Quick Reference

```bash
# Build and test
make clean && make test_all

# Check specific tests
./test_anchor_quant_edge
./test_anchor
./test_charclass

# View changes
git diff src/regjit.cpp

# Check for warnings
make 2>&1 | grep -i warning
```
