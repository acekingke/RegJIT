# RegJIT Python re Compatibility Report

## Summary

基础功能（181 项测试）全部通过。深度兼容性审查发现 9 类不兼容问题，其中 **3 项严重/高优先级问题已修复**（2026-02-11），剩余 6 项为中低优先级。

| Category | Tests | Passed | Failed | Compatibility |
|----------|-------|--------|--------|---------------|
| Basic Literals | 6 | 6 | 0 | ✅ 100% |
| Quantifiers | 23 | 23 | 0 | ✅ 100% |
| Anchors | 24 | 24 | 0 | ✅ 100% |
| Character Classes | 23 | 23 | 0 | ✅ 100% |
| Escape Sequences | 35 | 35 | 0 | ✅ 100% |
| Alternation | 13 | 13 | 0 | ✅ 100% |
| Groups | 9 | 9 | 0 | ✅ 100% |
| Syntax Errors | 21 | 21 | 0 | ✅ 100% |
| Combined Patterns | 27 | 27 | 0 | ✅ 100% |
| **基础测试合计** | **181** | **181** | **0** | **100%** |

### Previously Fixed Issues (2026-02-06)

1. **End Anchor (`$`) in Search Mode** - Fixed: `$` anchor now correctly verifies match position at end of string
2. **Word Boundary (`\b`)** - Fixed: Correctly handles word/non-word character transitions
3. **Non-Word Boundary (`\B`)** - Fixed: Correctly handles NO word/non-word character transitions

---

## 已修复的不兼容问题（2026-02-11）

### [已修复] 问题 1: 词法分析器静默跳过空白字符 [P0 严重]

**修复**: 删除了 `get_next_token()` 中的 `isspace()` 检查（`src/regjit.cpp`），空白字符现在作为 `CHAR` token 正常返回。

**修复前**: 模式 `" "` 不匹配空格，`"a b"` 等价于 `"ab"`。
**修复后**: 空白字符作为字面量参与匹配，与 Python re 一致。

### [已修复] 问题 3: `.` 错误排除 `\r` [P1 高]

**修复**: 在 `CharClass::CodeGen()` 的 dot 分支中移除了 `\r` 的排除检查（`src/regjit.cpp`），只排除 `\n`。

**修复前**: `.` 同时排除 `\n` 和 `\r`。
**修复后**: `.` 只排除 `\n`，与 Python re 一致。

### [已修复] 问题 4: 不支持空分支的选择操作 [P1 高]

**修复**: 重构了 `parse_expr()`（`src/regjit.cpp`），在遇到 `|` 后检测空分支（PIPE/RPAREN/EOS），返回空 `Concat` 节点匹配空字符串。同时在 `parse()` 中添加了剩余 token 检查，确保顶层的 `)` 仍报 "unbalanced parenthesis"。

**修复前**: `a|`、`|a`、`(a|)`、`(|)`、`()` 无法编译。
**修复后**: 这些模式均正常编译，空分支匹配空字符串，与 Python re 一致。

**测试更新**: `tests/test_syntax.cpp` 中 `test_empty_group()` 从期望 `()` 编译失败改为期望编译成功并匹配空字符串。

---

## 待修复的不兼容问题

### 问题 2: 不完整花括号被错误拒绝 [P2 中等]

**位置**: `src/regjit.cpp:1862-1863, 1887-1888`

**Python re 行为**: 不完整的花括号表达式被当作字面量字符处理：
```python
re.search(r'a{2', 'a{2')     # 匹配 'a{2' (字面量)
re.search(r'a{', 'a{')       # 匹配 'a{' (字面量)
re.search(r'{', '{')          # 匹配 '{' (字面量)
re.search(r'}', '}')          # 匹配 '}' (字面量)
re.search(r'a{b', 'a{b')     # 匹配 'a{b' (字面量)
re.search(r'a{2,b}', 'a{2,b}') # 匹配 (字面量)
```

**RegJIT 行为**: 词法分析器将 `{` 作为 `LBRACE` token 发出，解析器随即要求合法的量词格式，否则抛出错误：
- `a{2` -> `"Malformed quantifier: missing '}'"`
- `a{` -> `"Malformed quantifier: expected digit after '{'"`
- `a{b` -> `"Malformed quantifier: expected digit after '{'"`

**影响**: 包含字面花括号的正则表达式无法编译（例如匹配 JSON 格式的文本 `\{.*\}` 可以工作，但 `a{2` 不行）。

**修复方案**: 在解析器遇到不合法花括号格式时回退（backtrack），将 `{` 及其后续字符作为字面量处理，而非抛出异常。

---

### 问题 5: 不支持 `{,m}` 语法 [P2 中等]

**位置**: `src/regjit.cpp:1862-1863`

**Python re 行为**: `{,m}` 等价于 `{0,m}`：
```python
re.search(r'a{,3}', 'aaa')  # 匹配 span=(0, 3)
re.search(r'a{,3}', '')     # 匹配 span=(0, 0)
re.search(r'a{,}', 'aaa')   # 匹配 span=(0, 3)，等价于 a*
```

**RegJIT 行为**: 解析器在 `{` 后要求第一个字符是数字，遇到逗号直接抛出：
```
"Malformed quantifier: expected digit after '{'"
```

**影响**: 使用 `{,m}` 简写的正则表达式无法编译。

**修复方案**: 在解析 `{` 后允许省略 min（默认为 0），即先检查是否为逗号，如果是则 min=0。

---

### 问题 6: 不支持占有量词 `a++`、`a*+`、`a?+` [低]

**位置**: `src/regjit.cpp:1802-1803`

**Python 3.11+ 行为**: 支持占有量词（possessive quantifier），贪婪匹配且不回溯：
```python
re.search(r'a++', 'aaa')    # 匹配 span=(0, 3)
re.search(r'a*+', 'aaa')    # 匹配 span=(0, 3)
re.search(r'a?+', 'a')      # 匹配 span=(0, 1)
re.search(r'a{2,4}+', 'aaa') # 匹配 span=(0, 3)
```

**RegJIT 行为**: 将量词后的 `+` 视为第二个量词，抛出 `"multiple repeat"` 错误。

**影响**: Python 3.11+ 中合法的占有量词无法编译。由于 RegJIT 当前不支持回溯，贪婪量词的行为本身已等同于占有量词，因此功能影响有限。

**修复方案**: 在 `parse_postfix()` 中检测量词后的 `+`，将其识别为占有量词标志（可以直接忽略该标志，因为当前贪婪行为已等效）。

---

### 问题 7: 不支持 `{0}` 和 `{0,0}` [低]

**Python re 行为**: `a{0}` 和 `a{0,0}` 匹配空字符串：
```python
re.search(r'a{0}', 'b')     # 匹配 span=(0, 0)
re.search(r'a{0,0}', 'xyz') # 匹配 span=(0, 0)
```

**RegJIT 行为**: 需要验证。`Repeat::makeRange(node, 0, 0, false)` 可能生成的循环逻辑中 min=0, max=0，行为取决于 `Repeat::CodeGen()` 的实现。如果 min 阶段循环 0 次且 max 阶段也是 0 次，应该能正常工作，但需要测试确认。

**修复方案**: 添加测试验证；如果不工作，在 `Repeat::CodeGen()` 中对 min=0, max=0 特判为直接成功。

---

### 问题 8: 字符类中特殊位置的 `-` 处理 [低]

**Python re 行为**: `-` 出现在字符类的开头或结尾时作为字面量：
```python
re.search(r'[-]', '-')    # 匹配
re.search(r'[-a]', '-')   # 匹配
re.search(r'[a-]', '-')   # 匹配
re.search(r'[a-]', 'a')   # 匹配
```

**RegJIT 行为**: 需要验证 `parse_character_class()` 对边界位置 `-` 的处理。如果 `-` 在类末尾（后接 `]`）或类开头时，解析器可能误将其解析为范围操作符并抛出错误。

**修复方案**: 验证现有行为；如有问题，在 `parse_character_class()` 中对 `-` 在首尾位置做字面量处理。

---

### 问题 9: 未实现的功能 [记录]

以下 Python re 功能尚未实现，这些不属于 bug 而是功能缺失：

| 功能 | Python 语法 | 状态 |
|------|------------|------|
| 捕获组/反向引用 | `(pattern)`, `\1` | 未实现 |
| 命名捕获组 | `(?P<name>pattern)` | 未实现 |
| 前向断言 | `(?=...)`, `(?!...)` | 未实现 |
| 后向断言 | `(?<=...)`, `(?<!...)` | 未实现 |
| 标志 | `re.IGNORECASE`, `re.MULTILINE`, `re.DOTALL`, `re.VERBOSE` | 未实现 |
| 内联标志 | `(?i)`, `(?m)`, `(?s)`, `(?x)` | 未实现 |
| 条件模式 | `(?(id)yes\|no)` | 未实现 |
| Unicode 属性 | `\p{L}`, 完整 Unicode 支持 | 未实现 |
| 原子组 | `(?>...)` (Python 3.11+) | 未实现 |

---

## 不兼容问题优先级排序

| 优先级 | 问题 | 修复难度 | 影响范围 |
|--------|------|----------|----------|
| **P0 严重** | #1 空白字符被跳过 | 简单（删除 3 行） | 所有含空格的模式 |
| **P1 高** | #3 `.` 错误排除 `\r` | 简单（删除 2 行） | Windows 文本处理 |
| **P1 高** | #4 不支持空分支选择 | 中等 | `a\|`, `\|a`, `(a\|)` 等模式 |
| **P2 中** | #2 花括号字面量回退 | 中等 | 含字面 `{` 的模式 |
| **P2 中** | #5 `{,m}` 语法 | 简单 | 省略 min 的量词 |
| **P3 低** | #6 占有量词 | 简单（可忽略标志） | Python 3.11+ 兼容 |
| **P3 低** | #7 `{0}` / `{0,0}` | 需验证 | 极少见用法 |
| **P3 低** | #8 字符类 `-` 边界 | 需验证 | 字符类边界情况 |

## Test Files

- `tests/test_python_re_compat.py` - Python-based compatibility test suite
- `tests/test_python_re_compat.cpp` - C++ compatibility test suite (181 basic tests)
- `tests/test_compat_check.cpp` - 深度兼容性检查（待加入 Makefile）

## How to Run Tests

```bash
# C++ basic tests (181 cases)
make src/regjit.o
c++ -g -O0 -Wall -std=c++17 $(llvm-config --cxxflags) -I./src \
    -o test_python_re_compat tests/test_python_re_compat.cpp src/regjit.o \
    $(llvm-config --ldflags --libs core orcjit native) -lpthread
./test_python_re_compat

# Python tests (requires Python bindings)
make libregjit.so && make python-bindings
DYLD_LIBRARY_PATH=. python3 tests/test_python_re_compat.py
```

---
Generated: 2026-02-06
Updated: 2026-02-11
Status: 基础测试 181/181 通过，深度审查发现 9 类不兼容问题
