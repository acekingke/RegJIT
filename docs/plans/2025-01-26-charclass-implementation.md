# Character Classes Implementation Plan

**Status:** Completed. Character class support is implemented in mainline.

**Summary**
- Lexer supports `[ ]`, `-`, `^` (class negation), and `.` tokens.
- Parser builds `CharClass` nodes and ranges.
- Codegen handles dot class and negated ranges.

**Key Files**
- `src/regjit.cpp`
- `src/regjit.h`
- `tests/test_charclass.cpp`

**Tests**
```bash
make test_charclass && ./test_charclass
```

---

## Original Plan (Archived)

# Character Classes Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add comprehensive character class support to RegJIT including [abc], [a-z], [^abc], and . patterns.

**Architecture:** Extend the existing AST system with CharClass nodes, enhance the lexer to recognize bracket syntax, and optimize LLVM code generation using bitmaps and SIMD instructions.

**Tech Stack:** LLVM 19+, C++17, CMake, Google Test (for comprehensive testing)

---

## Task 1: Setup Development Environment

**Files:**
- Create: `../regjit-charclass/` (git worktree)
- Modify: `Makefile` (add test targets)

**Step 1: Create isolated worktree**

```bash
cd /Users/kyc/homework/tmp
git worktree add regjit-charclass -b feature/charclass
cd regjit-charclass
```

**Step 2: Verify environment**

Run: `ls -la`
Expected: Show RegJIT project files in isolated worktree

**Step 3: Create test framework structure**

```bash
mkdir -p tests/charclass
touch tests/charclass/test_charclass.cpp
```

**Step 4: Update Makefile for testing**

```makefile
# Add to existing Makefile
test-charclass: $(BUILD_DIR)/test_charclass
	./$(BUILD_DIR)/test_charclass

$(BUILD_DIR)/test_charclass: tests/charclass/test_charclass.cpp $(BUILD_DIR)/regjit.o
	$(CXX) $(CXXFLAGS) -I$(LLVM_INCLUDE) $^ -o $@ $(LLVM_LIBS)
```

**Step 5: Commit setup**

```bash
git add tests/charclass/ Makefile
git commit -m "feat: setup charclass development environment"
```

---

## Task 2: Write Failing Tests for Character Classes

**Files:**
- Create: `tests/charclass/test_charclass.cpp`

**Step 1: Write comprehensive test suite**

```cpp
#include "../src/regjit.h"
#include <cassert>
#include <iostream>

void test_basic_charclass() {
    Initialize();
    CompileRegex("[abc]");
    assert(Execute("a") == 1);
    assert(Execute("b") == 1);
    assert(Execute("c") == 1);
    assert(Execute("d") == 0);
    CleanUp();
}

void test_charclass_range() {
    Initialize();
    CompileRegex("[a-z]");
    assert(Execute("m") == 1);
    assert(Execute("z") == 1);
    assert(Execute("A") == 0);
    assert(Execute("5") == 0);
    CleanUp();
}

void test_negated_charclass() {
    Initialize();
    CompileRegex("[^abc]");
    assert(Execute("d") == 1);
    assert(Execute("x") == 1);
    assert(Execute("a") == 0);
    assert(Execute("b") == 0);
    CleanUp();
}

void test_dot_wildcard() {
    Initialize();
    CompileRegex("a.c");
    assert(Execute("abc") == 1);
    assert(Execute("axc") == 1);
    assert(Execute("ac") == 0);
    assert(Execute("a\n") == 0); // dot doesn't match newline
    CleanUp();
}

void test_complex_charclass() {
    Initialize();
    CompileRegex("[a-zA-Z0-9]");
    assert(Execute("Z") == 1);
    assert(Execute("5") == 1);
    assert(Execute("a") == 1);
    assert(Execute("@") == 0);
    CleanUp();
}

int main() {
    test_basic_charclass();
    test_charclass_range();
    test_negated_charclass();
    test_dot_wildcard();
    test_complex_charclass();
    std::cout << "All character class tests passed!" << std::endl;
    return 0;
}
```

**Step 2: Run test to verify it fails**

Run: `make test-charclass`
Expected: Compilation errors - CharClass not implemented

**Step 3: Commit test framework**

```bash
git add tests/charclass/test_charclass.cpp
git commit -m "test: add comprehensive charclass test suite"
```

---

## Task 3: Extend Lexer for Character Classes

**Files:**
- Modify: `src/regjit.cpp` (RegexLexer class)

**Step 1: Add new token types**

```cpp
// Add to TokenType enum in RegexLexer
enum TokenType {
    CHAR,
    STAR, PLUS, QMARK, PIPE, LPAREN, RPAREN,
    LBRACKET, RBRACKET, DASH, CARET, DOT,  // New tokens
    EOS
};
```

**Step 2: Implement bracket parsing logic**

```cpp
Token get_next_token() {
    while (m_cur_char != '\0') {
        switch (m_cur_char) {
            // ... existing cases ...
            case '[': next(); return {LBRACKET, '['};
            case ']': next(); return {RBRACKET, ']'};
            case '-': next(); return {DASH, '-'};
            case '^': next(); return {CARET, '^'};
            case '.': next(); return {DOT, '.'};
            default: {
                char c = current();
                next();
                return {CHAR, c};
            }
        }
    }
    return {EOS, '\0'};
}
```

**Step 3: Add character class content parsing**

```cpp
private:
    std::string parse_charclass_content() {
        std::string content;
        while (m_cur_char != ']' && m_cur_char != '\0') {
            content += m_cur_char;
            next();
        }
        return content;
    }

public:
    Token get_charclass_token() {
        next(); // Skip '['
        bool negated = false;
        if (m_cur_char == '^') {
            negated = true;
            next();
        }
        
        std::string content = parse_charclass_content();
        if (m_cur_char == ']') {
            next(); // Skip ']'
        }
        
        return {CHARCLASS, static_cast<char>(negated), content};
    }
```

**Step 4: Update main token recognition**

```cpp
Token get_next_token() {
