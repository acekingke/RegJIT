# 🚀 RegJIT - Regular Expression JIT Compiler

A high-performance regex engine that compiles regular expressions to native machine code using LLVM, achieving **up to 231x speedup** over C++'s `std::regex`.

## 📊 Performance Results

Benchmark results comparing RegJIT vs `std::regex` (compiled with `-O3`):

| Test Case | Pattern | JIT (ns) | std::regex (ns) | Speedup |
|-----------|---------|----------|-----------------|---------|
| Character class | `[a-zA-Z0-9]+` | 8 | 1848 | **231x** |
| Nested groups | `(a(b(c)+)+)+` | 7 | 1618 | **231x** |
| Word chars | `\w+` | 6 | 1164 | **194x** |
| Digits | `\d+` | 5 | 884 | **177x** |
| Alternation | `cat\|dog\|bird` | 10 | 1511 | **151x** |
| Email pattern | `[a-z]+@[a-z]+\.[a-z]+` | 38 | 5248 | **138x** |
| IP pattern | `\d+\.\d+\.\d+\.\d+` | 18 | 1944 | **108x** |
| Whitespace | `\s+` | 6 | 587 | **98x** |
| Plus quantifier | `a+` (1000 chars) | 609 | 56779 | **93x** |
| Star quantifier | `a*` (1000 chars) | 668 | 56938 | **85x** |
| Long input search | `needle` (10K chars) | 5902 | 354027 | **60x** |

**Average Speedup: 94.9x**

## ✨ Key Features

- **Native Machine Code**: Compiles regex patterns directly to CPU instructions
- **LLVM Optimization**: Leverages LLVM's O2 optimization pipeline
- **Pattern-Specific Code**: Each pattern gets its own optimized binary
- **Zero-Copy Matching**: Direct pointer manipulation without memory allocation
- **Python Bindings**: Full Python 3.12 integration with caching

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
# Build optimized benchmark
make clean && make RELEASE=1 bench

# Run benchmark
./bench
```

Example output:
```
====================================================================================================
                           RegJIT vs std::regex Benchmark Results
====================================================================================================
Test Case                Pattern                 JIT (ns)     std::regex     Speedup
----------------------------------------------------------------------------------------------------
Simple literal           hello                          9            137      15.2x
Char class [a-zA-Z0-9]+  [a-zA-Z0-9]+                   8           1848     231.0x
Digit \d+                \d+                            5            884     176.8x
Email-like pattern       [a-z]+@[a-z]+\.[a-z]+          38           5248     138.1x
...
----------------------------------------------------------------------------------------------------
Average Speedup:                                                                        94.9x
====================================================================================================
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
