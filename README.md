# 🚀 RegJIT - Regular Expression JIT Compiler

A high-performance regex engine that compiles regular expressions to native machine code using LLVM, achieving **up to 1031x speedup** over C++'s `std::regex` and **up to 8.5x speedup** over PCRE2-JIT.

## 📊 Performance Results

Benchmark results comparing RegJIT vs `std::regex` vs PCRE2-JIT (compiled with `-O3`, 10K iterations):

| Test Case | Pattern | RegJIT (ns) | std::regex (ns) | PCRE2-JIT (ns) | vs std | vs PCRE2 |
|-----------|---------|-------------|-----------------|----------------|--------|----------|
| Plus quantifier | `a+` | 55 | 56,759 | 346 | **1031x** | **6.3x** |
| Star quantifier | `a*` | 59 | 56,705 | 317 | **961x** | **5.4x** |
| Long input (10KB) | `needle` | 434 | 351,682 | 291 | **810x** | 0.67x |
| Char class | `[a-zA-Z0-9]+` | 3 | 1,935 | 22 | **645x** | **7.3x** |
| Word chars | `\w+` | 2 | 1,255 | 17 | **628x** | **8.5x** |
| Digits | `\d+` | 2 | 939 | 15 | **470x** | **7.5x** |
| Whitespace | `\s+` | 2 | 664 | 14 | **332x** | **7.0x** |
| Char class | `[a-z]+` | 2 | 442 | 12 | **221x** | **6.0x** |
| Negated class | `[^0-9]+` | 2 | 416 | 13 | **208x** | **6.5x** |
| Email pattern | `[a-z]+@[a-z]+\.[a-z]+` | 27 | 5,325 | 23 | **197x** | 0.85x |
| Alternation | `cat\|dog\|bird` | 8 | 1,534 | 14 | **192x** | **1.8x** |
| IP pattern | `\d+\.\d+\.\d+\.\d+` | 11 | 2,020 | 24 | **184x** | **2.2x** |
| Nested groups | `(a(b(c)+)+)+` | 12 | 1,624 | 40 | **135x** | **3.3x** |
| Exact repeat | `a{1000}` | 87 | 8,296 | 323 | **95x** | **3.7x** |

**Average Speedup: 317x vs std::regex, 3.9x vs PCRE2-JIT**

### Key Optimizations

- **Boyer-Moore-Horspool**: Custom string search algorithm (replaces slow macOS `memmem`)
- **ARM NEON SIMD**: Vectorized character counting for `a+`, `a*`, `a{n}` patterns
- **Direct Function Pointers**: Embedded libc calls (strlen, memchr) avoid symbol lookup
- **Fast Paths**: Single-char quantifiers use optimized counting instead of loops

## ✨ Key Features

- **Native Machine Code**: Compiles regex patterns directly to CPU instructions
- **LLVM Optimization**: Leverages LLVM's O2 optimization pipeline
- **SIMD Acceleration**: ARM NEON vectorized character counting
- **Boyer-Moore-Horspool**: Fast string search for literal patterns
- **Pattern-Specific Code**: Each pattern gets its own optimized binary
- **Zero-Copy Matching**: Direct pointer manipulation without memory allocation
- **Python Bindings**: Full Python 3.12 integration with LRU caching

## 🛠️ Supported Syntax

| Feature | Syntax | Example |
|---------|--------|---------|
| Literals | `abc` | `hello` |
| Quantifiers | `*`, `+`, `?`, `{n}`, `{n,}`, `{n,m}` | `a+`, `b{2,5}` |
| Character classes | `[abc]`, `[a-z]`, `[^0-9]` | `[a-zA-Z0-9_]+` |
| Dot wildcard | `.` | `a.b` |
| Alternation | `\|` | `cat\|dog` |
| Grouping | `(...)` | `(ab)+` |
| Anchors | `^`, `$`, `\b`, `\B` | `^hello$` |
| Escape sequences | `\d`, `\D`, `\w`, `\W`, `\s`, `\S` | `\d+\.\d+` |
| Special chars | `\t`, `\n`, `\r` | `line\nbreak` |

## 📥 Prerequisites

- LLVM 19+ (with development headers)
- C++17 compiler (GCC 10+ / Clang 12+ / MSVC 2019+)
- Python 3.12 (for Python bindings)

### LLVM Installation

**Ubuntu/Debian:**
```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 19
sudo apt-get install llvm-19-dev
```

**macOS (Homebrew):**
```bash
brew install llvm@19
export PATH="/opt/homebrew/opt/llvm@19/bin:$PATH"
```

## 🔧 Build Instructions

```bash
# Build with optimizations (recommended for benchmarks)
make RELEASE=1 bench

# Build tests
make test_all

# Build Python bindings
make libregjit.so
make python-bindings
```

### Build Options

| Option | Description |
|--------|-------------|
| `RELEASE=1` | Optimized build with `-O3` (recommended for benchmarks) |
| `REGJIT_DEBUG=1` | Enable IR dump and diagnostic output |

## 🧪 Usage

### C++ API

```cpp
#include "regjit.h"

int main() {
    Initialize();
    
    // Compile and execute
    if (CompileRegex("hello|world")) {
        auto sym = ExitOnErr(JIT->lookup(FunctionName));
        auto match = (int (*)(const char*))sym.getValue();
        
        printf("Match: %d\n", match("hello there"));  // 1
        printf("Match: %d\n", match("goodbye"));      // 0
    }
    
    CleanUp();
    return 0;
}
```

### Python API

```python
import sys
sys.path.insert(0, 'python')
from _regjit import Regex

# Compile and match
r = Regex(r'\d+\.\d+')
print(r.match('3.14'))      # True
print(r.match('hello'))     # False

# Pattern caching
import _regjit
print(_regjit.cache_size())  # Number of cached patterns
```

## 📈 Running Benchmarks

```bash
# Build optimized benchmark (requires PCRE2)
make clean && make RELEASE=1 bench

# Run benchmark
./bench
```

Example output:
```
========================================================================================================================
                         RegJIT vs std::regex vs PCRE2 (JIT) Benchmark Results
========================================================================================================================
Test Case             Pattern             RegJIT(ns)    std::regex     PCRE2-JIT        vs std      vs PCRE2
------------------------------------------------------------------------------------------------------------------------
Plus quantifier a+    a+                          55         56759           346       1031.98x        6.29x
Star quantifier a*    a*                          59         56705           317        961.10x        5.37x
Long input search     needle                     434        351682           291        810.33x        0.67x
Char class [a-zA-Z0-9]+[a-zA-Z0-9]+                 3          1935            22        645.00x        7.33x
Word \w+              \w+                          2          1255            17        627.50x        8.50x
Digit \d+             \d+                          2           939            15        469.50x        7.50x
...
------------------------------------------------------------------------------------------------------------------------
Average Speedup:                                                                        316.90x        3.91x
========================================================================================================================
```

## 🧠 Architecture

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   Pattern   │ ──▶ │   Parser    │ ──▶ │     AST     │ ──▶ │  LLVM IR    │
│   String    │     │   (Lexer)   │     │   Nodes     │     │  CodeGen    │
└─────────────┘     └─────────────┘     └─────────────┘     └─────────────┘
                                                                   │
                                                                   ▼
┌─────────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   Execute   │ ◀── │   Native    │ ◀── │  LLVM ORC   │ ◀── │  Optimize   │
│   Match()   │     │   Code      │     │    JIT      │     │    (O2)     │
└─────────────┘     └─────────────┘     └─────────────┘     └─────────────┘
```

### Core Components

1. **Lexer/Parser**: Tokenizes and parses regex patterns into AST
2. **AST Nodes**: `Match`, `Concat`, `Alternative`, `Repeat`, `CharClass`, `Anchor`
3. **CodeGen**: Generates LLVM IR from AST with control flow optimization
4. **JIT Compiler**: LLVM ORC JIT compiles IR to native machine code
5. **Cache**: LRU cache for compiled patterns with reference counting

## 🧪 Testing

```bash
# Run all tests
make test_all

# Run specific tests
make test_anchor && ./test_anchor
make test_charclass && ./test_charclass
make test_quantifier && ./test_quantifier
make test_escape && ./test_escape
make test_syntax && ./test_syntax

# Run Python tests
cd python/tests && python3.12 test_bindings.py
```

## 📄 License

MIT License

## 🤝 Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.
