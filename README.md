# 🚀 RegJIT - Regular Expression JIT Compiler

A high-performance regex engine that compiles regular expressions to native machine code using LLVM, achieving **up to 997x speedup** over C++'s `std::regex` and **up to 11x speedup** over PCRE2-JIT.

## 📊 Performance Results

Benchmark results comparing RegJIT vs `std::regex` vs PCRE2-JIT (compiled with `-O3`, 10K iterations):

| Test Case | Pattern | RegJIT (ns) | std::regex (ns) | PCRE2-JIT (ns) | vs std | vs PCRE2 |
|-----------|---------|-------------|-----------------|----------------|--------|----------|
| Plus quantifier | `a+` | 56 | 55,820 | 337 | **997x** | **6.0x** |
| Star quantifier | `a*` | 58 | 57,088 | 313 | **984x** | **5.4x** |
| Char class | `[a-zA-Z0-9]+` | 2 | 1,957 | 22 | **979x** | **11.0x** |
| Long input (10KB) | `needle` | 428 | 360,640 | 326 | **843x** | 0.76x |
| Word chars | `\w+` | 2 | 1,241 | 17 | **621x** | **8.5x** |
| Digits | `\d+` | 2 | 880 | 15 | **440x** | **7.5x** |
| Whitespace | `\s+` | 2 | 632 | 15 | **316x** | **7.5x** |
| Char class | `[a-z]+` | 2 | 452 | 13 | **226x** | **6.5x** |
| Negated class | `[^0-9]+` | 2 | 424 | 15 | **212x** | **7.5x** |
| Alternation | `cat|dog|bird` | 8 | 1,611 | 15 | **201x** | **1.9x** |
| Email pattern | `[a-z]+@[a-z]+\.[a-z]+` | 32 | 5,407 | 23 | **169x** | 0.72x |
| IP pattern | `\d+\.\d+\.\d+\.\d+` | 12 | 1,978 | 24 | **165x** | **2.0x** |
| Exact repeat | `a{1000}` | 58 | 7,559 | 345 | **130x** | **6.0x** |
| Nested groups | `(a(b(c)+)+)+` | 15 | 1,653 | 43 | **110x** | **2.9x** |

**Average Speedup: 331x vs std::regex, 4.3x vs PCRE2-JIT**

### Key Optimizations

- **memchr-Accelerated Search**: Uses `memchr` to find required characters in patterns (e.g., `@` in email patterns)
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
| Alternation | `\|` | `cat|dog` |
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

### C++ Benchmark (RegJIT vs std::regex vs PCRE2)

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
Plus quantifier a+    a+                          56         55820           337        996.79x        6.02x
Star quantifier a*    a*                          58         57088           313        984.28x        5.40x
Long input search     needle                     428        360640           326        842.62x        0.76x
Char class [a-zA-Z0-9]+[a-zA-Z0-9]+                 2          1957            22        978.50x       11.00x
Word \w+              \w+                          2          1241            17        620.50x        8.50x
Digit \d+             \d+                          2           880            15        440.00x        7.50x
...
------------------------------------------------------------------------------------------------------------------------
Average Speedup:                                                                        330.80x        4.30x
========================================================================================================================
```

### Python Benchmark (RegJIT vs Python re)

```bash
# Build Python bindings first
make libregjit.so
make python-bindings

# Run Python benchmark
cd python && DYLD_LIBRARY_PATH=.. python3.12 benchmark.py
```

**Important**: The Python benchmark shows RegJIT is slower than Python's `re` module (~0.1x average). This is NOT a RegJIT issue — it's a **Python/C++ boundary overhead** problem:

| Overhead Source | Estimated Cost |
|-----------------|----------------|
| Python → C++ string conversion | ~1000ns |
| C function call (acquire/release) | ~500ns |
| C++ → Python object conversion | ~1500ns |
| Exception handling setup | ~500ns |
| **Total per call** | **~3500ns** |

For simple patterns where RegJIT's actual matching takes ~10-50ns, this 3500ns overhead completely dominates.

**What We Did**: Optimized the Python bindings to cache the JIT function pointer at construction time, eliminating acquire/release overhead per call (~10% improvement).

**Conclusion**: Python bindings are designed for **convenience and integration**, not maximum performance. For production systems requiring peak regex speed, use the C++ API directly.

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
