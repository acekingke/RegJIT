---
name: regjit-testing
description: Use when writing tests for RegJIT features, verifying compatibility with PCRE/RE2, and ensuring code quality
---

# RegJIT Testing

## Overview

Write comprehensive tests for RegJIT regex engine to ensure correctness, prevent regressions, and verify compatibility with reference engines (PCRE, std::regex, RE2).

## When to Use

- After implementing new features
- Before committing code
- When fixing bugs (write test first)
- When refactoring regex behavior
- When verifying anchor/quantifier edge cases

## Test Types

### Unit Tests
Test individual components in isolation

**Example: Character matching**
```cpp
Initialize();
CompileRegex("a");
assert(Execute("a") == 1);      // Match
assert(Execute("b") == 0);      // No match
CleanUp();
```

### Integration Tests
Test full compilation pipeline with realistic patterns

**Example: Anchor quantifier**
```cpp
Initialize();
CompileRegex("^+");
assert(Execute("") == 1);       // Match at start
assert(Execute("xyz") == 1);    // Match at start
CleanUp();
```

### Edge Case Tests
Test boundary conditions and special scenarios

**Example: Empty string and boundaries**
```cpp
Initialize();
CompileRegex("^$");
assert(Execute("") == 1);       // Match empty
assert(Execute("x") == 0);      // Non-empty fails
CleanUp();
```

## RegJIT Test Structure

### Template
```cpp
#include "../src/regjit.h"
#include <iostream>
#include <cassert>

int main() {
    std::cout << "[Feature Name Tests]" << std::endl;
    
    // Test case 1
    Initialize();
    CompileRegex("pattern");
    assert(Execute("input") == expected);
    CleanUp();
    
    // Test case 2
    Initialize();
    CompileRegex("pattern2");
    assert(Execute("input2") == expected2);
    CleanUp();
    
    std::cout << "[All tests passed]" << std::endl;
    return 0;
}
```

### Test File Naming
- `test_anchor.cpp` - Anchor features (`^`, `$`, `\b`, `\B`)
- `test_charclass.cpp` - Character classes (`[abc]`, `[a-z]`)
- `test_quantifier.cpp` - Quantifiers (`*`, `+`, `{n}`)
- `test_anchor_quant_edge.cpp` - Anchor + quantifier combinations

## Test Categories for RegJIT

### 1. Anchor Tests
```
✅ ^ (start anchor)
   - Matches at position 0
   - With quantifiers: ^*, ^+, ^{n}
   
✅ $ (end anchor)
   - Matches at string length
   - With quantifiers: $*, $+, ${n}
   
✅ \b (word boundary)
   - Between \w and \W characters
   - With quantifiers: \b*, \b+
   
✅ \B (non-word boundary)
   - NOT between \w and \W
   - With quantifiers: \B*, \B+
```

### 2. Character Class Tests
```
✅ [abc] - Character set
✅ [a-z] - Range matching
✅ [^abc] - Negated class
✅ [a-zA-Z0-9] - Multiple ranges
✅ . - Any character
```

### 3. Quantifier Tests
```
✅ * - Zero or more (greedy)
✅ + - One or more (greedy)
✅ ? - Zero or one
✅ {n} - Exactly n times
✅ {n,} - At least n times
✅ {n,m} - Between n and m times
✅ *? - Zero or more (non-greedy)
✅ +? - One or more (non-greedy)
```

### 4. Combination Tests
```
✅ Anchors + Quantifiers
✅ Character class + Quantifiers
✅ Alternation + Anchors
✅ Groups + Features
```

## Writing Good Tests

### Principle 1: Clear Intent
```cpp
// GOOD - Intent clear
assert(Execute("a") == 1);      // Single char matches

// UNCLEAR - Intent obscured
assert(1 == Execute("a"));      // Backwards, hard to read
```

### Principle 2: Test One Thing
```cpp
// GOOD - Single behavior
Initialize();
CompileRegex("a+");
assert(Execute("aaa") == 1);
CleanUp();

// WRONG - Multiple behaviors mixed
Initialize();
CompileRegex("a+");
assert(Execute("aaa") == 1 && Execute("b") == 0);
CleanUp();
```

### Principle 3: Edge Cases Matter
```cpp
// Pattern: [a-z]+
Initialize();
CompileRegex("[a-z]+");
assert(Execute("") == 0);           // Empty
assert(Execute("a") == 1);          // Minimum
assert(Execute("abc") == 1);        // Normal
assert(Execute("z") == 1);          // Boundary
assert(Execute("A") == 0);          // Outside range
assert(Execute("a1b") == 0);        // Non-matching char
CleanUp();
```

### Principle 4: Verify Against Reference
```cpp
// After writing test, verify with PCRE:
// pcre: [a-z]+ matches "abc" ✓
// pcre: [a-z]+ does NOT match "A" ✓
// Our implementation matches PCRE ✓
```

## Test Execution

### Building Tests
```bash
# Specific test
make test_anchor_quant_edge

# Run test
./test_anchor_quant_edge

# All tests
make test_all
```

### Common Commands
```bash
# Build specific test
make test_charclass

# Run after building
./test_charclass

# Clean and rebuild
make clean && make test_all

# View test output
make test_anchor 2>&1 | tail -20
```

## PCRE/RE2 Compatibility Testing

### Verification Checklist
- [ ] Test matches PCRE behavior
- [ ] Test matches std::regex behavior
- [ ] Test matches RE2 behavior
- [ ] Document any intentional differences
- [ ] Add test to RegJIT suite

### Example: Anchor Quantifier Compatibility
```cpp
// Pattern: $+
// Expected: Matches only at end, but attempts at all offsets

// PCRE: $+ on "abc" matches at position 3 ✓
// std::regex: $+ on "abc" matches ✓
// RE2: $+ on "abc" matches ✓
// RegJIT: Our implementation matches ✓

Initialize();
CompileRegex("$+");
assert(Execute("abc") == 1);
CleanUp();
```

## Test Maintenance

### When to Add Tests
- [ ] Every new feature gets tests
- [ ] Every bug fix includes regression test
- [ ] Every compatibility change verified
- [ ] Every optimization needs performance test

### When to Update Tests
- [ ] When refactoring matching logic
- [ ] When changing quantifier behavior
- [ ] When modifying anchor semantics
- [ ] When fixing a bug

### When to Remove Tests
- [ ] Dead features removed
- [ ] Test infrastructure changed
- [ ] Duplicate test coverage consolidated

## Quick Reference

| Pattern | Test Files | Purpose |
|---------|-----------|---------|
| Anchors | test_anchor.cpp | `^` `$` `\b` `\B` |
| Charclass | test_charclass.cpp | `[...]` `.` |
| Quantifiers | test_quantifier.cpp | `*` `+` `?` `{n}` |
| Edge Cases | test_anchor_quant_edge.cpp | Complex combinations |

## Success Criteria

- [ ] All tests compile without warnings
- [ ] All tests pass (exit code 0)
- [ ] No memory leaks
- [ ] Tests are independent
- [ ] Coverage > 80%
- [ ] PCRE/RE2 compatible
- [ ] Comments explain purpose
- [ ] Ready to commit
