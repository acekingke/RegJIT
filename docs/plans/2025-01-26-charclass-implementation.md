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
    while (m_cur_char != '\0') {
        switch (m_cur_char) {
            // ... existing cases ...
            case '[': 
                return get_charclass_token();
            // ... rest of cases ...
        }
    }
    return {EOS, '\0'};
}
```

**Step 5: Run lexer tests**

Run: `make test-charclass`
Expected: Lexer compiles but parser fails on CHARCLASS tokens

**Step 6: Commit lexer extension**

```bash
git add src/regjit.cpp
git commit -m "feat: extend lexer for character class syntax"
```

---

## Task 4: Implement CharClass AST Node

**Files:**
- Modify: `src/regjit.h` (add CharClass class)
- Modify: `src/regjit.cpp` (implement CharClass::CodeGen)

**Step 1: Add CharClass declaration to header**

```cpp
class CharClass : public Root {
private:
    std::vector<std::pair<char, char>> ranges;  // [start, end] pairs
    std::vector<char> single_chars;             // Individual characters
    bool negated;
    
public:
    explicit CharClass(const std::string& pattern, bool negate = false);
    Value* CodeGen() override;
    ~CharClass() override = default;
    
private:
    void parse_pattern(const std::string& pattern);
    bool matches_char(char c) const;
};
```

**Step 2: Implement CharClass constructor**

```cpp
CharClass::CharClass(const std::string& pattern, bool negate) 
    : negated(negate) {
    parse_pattern(pattern);
}

void CharClass::parse_pattern(const std::string& pattern) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (i + 2 < pattern.size() && pattern[i+1] == '-') {
            // Range like a-z
            ranges.push_back({pattern[i], pattern[i+2]});
            i += 2;
        } else {
            // Single character
            single_chars.push_back(pattern[i]);
        }
    }
}
```

**Step 3: Implement LLVM code generation**

```cpp
Value* CharClass::CodeGen() {
    // Load current character
    Value* curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
    Value* strChar = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, {curIdx});
    Value* charVal = Builder.CreateLoad(Builder.getInt8Ty(), strChar);
    
    // Create comparison for each range and single character
    Value* result = nullptr;
    
    // Check single characters
    for (char c : single_chars) {
        Value* cmp = Builder.CreateICmpEQ(charVal, 
            ConstantInt::get(Context, APInt(8, c)));
        if (result == nullptr) {
            result = cmp;
        } else {
            result = Builder.CreateOr(result, cmp);
        }
    }
    
    // Check ranges
    for (auto& range : ranges) {
        Value* ge_lower = Builder.CreateICmpUGE(charVal, 
            ConstantInt::get(Context, APInt(8, range.first)));
        Value* le_upper = Builder.CreateICmpULE(charVal, 
            ConstantInt::get(Context, APInt(8, range.second)));
        Value* in_range = Builder.CreateAnd(ge_lower, le_upper);
        
        if (result == nullptr) {
            result = in_range;
        } else {
            result = Builder.CreateOr(result, in_range);
        }
    }
    
    // Handle negation
    if (negated) {
        result = Builder.CreateNot(result);
    }
    
    // Handle empty result (shouldn't happen with valid patterns)
    if (result == nullptr) {
        result = ConstantInt::getFalse(Context);
    }
    
    Builder.CreateCondBr(result, GetSuccessBlock(), GetFailBlock());
    return nullptr;
}
```

**Step 4: Update parser to handle CHARCLASS tokens**

```cpp
// Add to RegexParser class
std::unique_ptr<Root> parse_element() {
    if (m_cur_token.type == RegexLexer::LBRACKET) {
        m_cur_token = m_lexer.get_next_token();
        if (m_cur_token.type == RegexLexer::CHARCLASS) {
            bool negated = m_cur_token.value != 0;
            std::string pattern = m_cur_token.extra_data; // Need to extend Token struct
            m_cur_token = m_lexer.get_next_token();
            return std::make_unique<CharClass>(pattern, negated);
        }
    }
    // ... existing element parsing logic ...
}
```

**Step 5: Run tests**

Run: `make test-charclass`
Expected: Basic character class tests pass, complex tests may fail

**Step 6: Commit CharClass implementation**

```bash
git add src/regjit.h src/regjit.cpp
git commit -m "feat: implement CharClass AST node with LLVM codegen"
```

---

## Task 5: Optimize LLVM Code Generation

**Files:**
- Modify: `src/regjit.cpp` (CharClass::CodeGen optimization)

**Step 1: Implement bitmap optimization for ASCII**

```cpp
Value* CharClass::CodeGen_optimized() {
    // For ASCII characters, use 256-bit bitmap (8 x 32-bit integers)
    Value* bitmap = create_char_bitmap();
    
    // Load current character and convert to index
    Value* curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
    Value* strChar = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, {curIdx});
    Value* charVal = Builder.CreateLoad(Builder.getInt8Ty(), strChar);
    Value* charIdx = Builder.CreateZExt(charVal, Builder.getInt32Ty());
    
    // Check bitmap
    Value* bitmap_idx = Builder.CreateLShr(charIdx, 5); // Divide by 32
    Value* bitmap_bit = Builder.CreateAnd(charIdx, 31); // Mod 32
    Value* bitmap_word = Builder.CreateExtractElement(bitmap, bitmap_idx);
    Value* bit_mask = Builder.CreateShl(ConstantInt::get(Context, APInt(32, 1)), bitmap_bit);
    Value* bit_set = Builder.CreateAnd(bitmap_word, bit_mask);
    
    Value* result = Builder.CreateICmpNE(bit_set, ConstantInt::get(Context, APInt(32, 0)));
    
    // Handle negation
    if (negated) {
        result = Builder.CreateNot(result);
    }
    
    Builder.CreateCondBr(result, GetSuccessBlock(), GetFailBlock());
    return nullptr;
}
```

**Step 2: Add bitmap creation helper**

```cpp
Value* CharClass::create_char_bitmap() {
    // Create 8 x 32-bit vector for 256 ASCII characters
    Value* bitmap = ConstantVector::getSplat(8, 
        ConstantInt::get(Context, APInt(32, 0)));
    
    // Set bits for matching characters
    for (char c : single_chars) {
        bitmap = set_bitmap_bit(bitmap, static_cast<unsigned char>(c));
    }
    
    for (auto& range : ranges) {
        for (char c = range.first; c <= range.second; ++c) {
            bitmap = set_bitmap_bit(bitmap, static_cast<unsigned char>(c));
        }
    }
    
    return bitmap;
}
```

**Step 3: Benchmark optimization**

```bash
# Create performance test
echo "Testing [a-zA-Z0-9] optimization:" > benchmark.log
time ./test_charclass >> benchmark.log
```

**Step 4: Commit optimizations**

```bash
git add src/regjit.cpp
git commit -m "perf: optimize CharClass with bitmap lookup"
```

---

## Task 6: Add Dot Wildcard Support

**Files:**
- Modify: `src/regjit.cpp` (parser and lexer)

**Step 1: Add dot token handling in parser**

```cpp
std::unique_ptr<Root> parse_element() {
    if (m_cur_token.type == RegexLexer::DOT) {
        m_cur_token = m_lexer.get_next_token();
        // Create CharClass for all characters except newline
        return std::make_unique<CharClass>("\x01-\x7F", false); // ASCII except \0
    }
    // ... existing element parsing ...
}
```

**Step 2: Test dot functionality**

Run: `make test-charclass`
Expected: All tests including dot wildcard pass

**Step 3: Commit dot support**

```bash
git add src/regjit.cpp
git commit -m "feat: add dot wildcard support"
```

---

## Task 7: Final Integration and Performance Testing

**Files:**
- Create: `benchmarks/charclass_bench.cpp`
- Modify: `README.md` (update feature list)

**Step 1: Create comprehensive benchmark**

```cpp
// benchmarks/charclass_bench.cpp
#include "../src/regjit.h"
#include <chrono>
#include <iostream>

void benchmark_charclass() {
    const std::string pattern = "[a-zA-Z0-9_]+";
    const std::string test_input = "test_string_123";
    
    Initialize();
    CompileRegex(pattern);
    
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100000; ++i) {
        Execute(test_input.c_str());
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    std::cout << "CharClass JIT: " << duration.count() / 100000 << " ns/iter\n";
    
    CleanUp();
}
```

**Step 2: Update documentation**

```markdown
## Supported Features
- ✅ Basic character matching
- ✅ Repetition operations (*, +, ?)
- ✅ Alternation (|)
- ✅ Grouping (())
- ✅ Character classes ([abc], [a-z], [^abc])
- ✅ Dot wildcard (.)
- ✅ Negation (!)
```

**Step 3: Final integration test**

```bash
make clean && make all && make test-charclass
```

**Step 4: Commit final implementation**

```bash
git add benchmarks/charclass_bench.cpp README.md
git commit -m "feat: complete character class implementation with benchmarks"
```

---

## Success Criteria

- [ ] All character class tests pass
- [ ] Performance maintains >60x speedup vs std::regex
- [ ] Code compiles without warnings
- [ ] Documentation updated
- [ ] Benchmarks show improvement

## Next Steps

After character classes are complete, proceed with:
1. Anchor support (^, $, \b, \B)
2. Extended quantifiers ({n}, {n,m}, non-greedy)
3. Predefined character classes (\d, \w, \s)