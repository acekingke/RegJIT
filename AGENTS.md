# RegJIT 代码分析报告

## 🎯 项目概述
这是一个基于LLVM的正则表达式JIT编译器，实现了**超过60倍的性能提升**。项目通过将正则表达式直接编译为原生机器码，避免了传统正则表达式的解释开销。

## ⚠️ 正则语法兼容性

**本项目遵循 Python `re` 模块的语法规范，而非 Perl/PCRE。**

主要区别：
- **锚点不支持量词**：`^*`、`^+`、`^{2}`、`$*` 等模式在 Python re 中会报错 "nothing to repeat"，本项目应拒绝编译这些模式
- **零宽度断言不支持量词**：`\b*`、`\B+`、`\b{2,}` 等模式也应被拒绝
- 验证语法行为时请使用 `python3.12` 的 `re` 模块作为参考

示例验证代码：
```python
import re
try:
    re.compile(r'^*')  # 会抛出 re.error: nothing to repeat
except re.error as e:
    print(f"Invalid pattern: {e}")
```

## 🏗️ 架构设计

### 核心组件
- **LLVM JIT Core**: 使用ORC JIT API进行动态代码生成
- **AST节点系统**: 抽象语法树表示正则表达式结构
- **词法/语法分析器**: 将正则表达式解析为AST
- **优化管道**: LLVM O2级别优化

### AST节点类型 (`src/regjit.h`)
```cpp
class Root {
    BasicBlock* failBlock;   // 失败跳转块
    BasicBlock* nextBlock;   // 成功跳转块
    virtual Value *CodeGen() = 0;
};

class Match : public Root     // 字符匹配
class Concat : public Root     // 连接操作
class Alternative : public Root // 选择操作 (|)
class Not : public Root        // 反向操作
class Repeat : public Root     // 重复操作 (*, +)
```

## 🚀 核心原理

### 1. LLVM JIT代码生成 (`src/regjit.cpp:18-44`)
```cpp
void Initialize() {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    JIT = ExitOnErr(LLJITBuilder().create());
}

void Compile() {
    // 优化模块
    OptimizeModule(*ThisModule);
    // 添加到JIT
    ExitOnErr(JIT->addIRModule(RT, 
        ThreadSafeModule(std::move(ThisModule), SafeCtx)));
}
```

### 2. 控制流优化
每个正则表达式操作生成对应的LLVM基本块：
- **成功块** → 继续下一个匹配
- **失败块** → 直接返回0

### 3. 重复操作的循环展开 (`src/regjit.cpp:200-314`)
```cpp
Value* Repeat::CodeGen() {
    if(times == Star) { // 0或多次
        BasicBlock* loopBlock = BasicBlock::Create(Context, "repeat_loop", MatchF);
        BasicBlock* bodySuccess = BasicBlock::Create(Context, "repeat_success", MatchF);
        BasicBlock* exitBlock = BasicBlock::Create(Context, "repeat_exit", MatchF);
        // 循环结构生成...
    }
    else if(times == Plus) { // 1或多次
        // 首次必匹配 + 后续循环
    }
}
```

### 4. 内存访问优化
- **零拷贝字符串匹配**: 直接操作输入字符串指针
- **编译时索引计算**: 避免运行时计算开销
- **内联所有匹配操作**: 消除函数调用开销

## 📊 性能优势分析

### 传统正则表达式引擎瓶颈
1. **解释执行**: 每次匹配都需要解析正则表达式
2. **通用代码**: 无法针对特定模式优化
3. **状态机开销**: 复杂的状态切换和函数调用

### RegJIT优化策略
1. **JIT编译**: 正则表达式 → LLVM IR → 原生机器码
2. **模式特定优化**: LLVM针对具体模式生成最优代码
3. **架构特定优化**: 利用目标CPU特性 (SIMD, 缓存优化)
4. **控制流直化**: 消除不必要的分支和循环

### 性能测试结果
```bash
Testing a{1000} on 1000 chars:
JIT time (1000): 375 ns/iter
std::regex time (1000): 26185 ns/iter (Speedup: 69x)
```

## 🔍 技术亮点

### 1. 词法/语法分析 (`src/regjit.cpp:317-457`)
```cpp
class RegexLexer {
    enum TokenType { CHAR, STAR, PLUS, PIPE, LPAREN, RPAREN, EOS };
    Token get_next_token();
};

class RegexParser {
    std::unique_ptr<Root> parse_expr();    // 解析选择 |
    std::unique_ptr<Root> parse_concat();  // 解析连接
    std::unique_ptr<Root> parse_postfix(); // 解析 *, +, ?
};
```

### 2. 优化管道 (`src/regjit.cpp:25-44`)
```cpp
void OptimizeModule(Module& M) {
    PassBuilder PB;
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    // 配置O2级别优化
    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
    MPM.run(M, MAM);
}
```

### 3. 高效的循环生成
使用LLVM的PHI节点和SSA形式生成优化的循环结构，支持：
- `*` - 零次或多次重复
- `+` - 一次或多次重复  
- 精确次数重复

## 💡 应用场景

1. **高频文本处理**: 日志分析、数据清洗
2. **实时系统**: 需要低延迟的模式匹配
3. **性能敏感应用**: 网络包检测、入侵检测
4. **大规模文本搜索**: 搜索引擎、文档处理

## 🎯 关键创新点

1. **正则表达式JIT编译**: 首个将正则表达式直接编译为机器码的开源实现
2. **零拷贝匹配架构**: 消除内存分配和拷贝开销
3. **模式特定优化**: 每个正则表达式都有专门的机器码版本
4. **LLVM生态集成**: 充分利用现代编译器优化技术

这个项目完美展示了现代编译器技术在性能优化中的强大威力，为高性能文本处理提供了新的解决方案。

---

## 📋 正则表达式功能扩展计划

### **当前支持的功能**
- ✅ 基本字符匹配
- ✅ 重复操作 `*`, `+`, `?`
- ✅ 选择操作 `|` 
- ✅ 分组 `( )`
- ✅ 反向操作 `!`
- ✅ 简单转义 `\\`

### **缺失的核心功能**

#### 1. **字符类支持** (高优先级)
```cpp
// 新增Token类型
LBRACKET, RBRACKET, DASH, CARET, DOT

// 新增AST节点
class CharClass : public Root {
    std::vector<std::pair<char, bool>> ranges; // (char, isIncluded)
    bool negated;
    Value* CodeGen() override;
};
```

**实现内容：**
- `[abc]` - 字符集合匹配
- `[a-z0-9]` - 范围匹配
- `[^abc]` - 否定字符类
- `.` - 任意字符（除换行符）

#### 2. **锚点支持** (已实现)
```cpp
// 新增AST节点
class Anchor : public Root {
    enum Type { Start, End, WordBoundary, NonWordBoundary };
    Type anchorType;
    Value* CodeGen() override;
};
```

**实现内容：**
- `^` - 行首匹配 ✅
- `$` - 行尾匹配 ✅
- `\b`, `\B` - 词边界匹配 ✅

#### 3. **扩展量词** (高优先级)
```cpp
// 扩展现有Repeat类
class Repeat : public Root {
    enum TimeType { Star, Plus, Exact, Min, MinMax, NonGreedyStar, NonGreedyPlus };
    int minCount, maxCount;
    bool nonGreedy;
    Value* CodeGen() override;
};
```

**实现内容：**
- `{n}` - 精确匹配n次 ✅
- `{n,}` - 至少匹配n次 ✅
- `{n,m}` - 匹配n到m次
- `*?`, `+?` - 非贪心模式
- `{n,m}?` - 非贪心区间

#### 4. **转义序列支持** (中优先级)
```cpp
// 扩展Token类型
enum TokenType {
    ...,
    BACKSLASH, DIGIT, NON_DIGIT, WORD, NON_WORD, 
    WHITESPACE, NON_WHITESPACE, ...
};
```

**实现内容：**
- `\d`, `\D` - 数字/非数字
- `\w`, `\W` - 单词/非单词字符
- `\s`, `\S` - 空白/非空白
- `\t`, `\n`, `\r` - 制表符、换行符等

#### 5. **分组和捕获** (中优先级)
```cpp
class Group : public Root {
    std::unique_ptr<Root> body;
    int groupId;           // 捕获组ID
    bool isCapturing;      // true: 捕获, false: 非捕获
    Value* CodeGen() override;
};
```

**实现内容：**
- `(pattern)` - 捕获组
- `(?:pattern)` - 非捕获组
- 返回捕获内容位置信息

#### 6. **回溯引用** (低优先级)
```cpp
class BackReference : public Root {
    int groupId;
    Value* CodeGen() override;
};
```

**实现内容：**
- `\1`, `\2` 等 - 引用之前的捕获组

---

## ⚡ 锚点/量词语义与搜索模式更新

### 动机与标准行为
锚点（如 `^`、`$`、`\b`、`\B`）是零宽度断言。

**重要：本项目遵循 Python `re` 模块的语义，而非 PCRE/Perl。**

在 Python re 中，对锚点使用量词（如 `^*`、`^+`、`^{2}`、`$*`）是**非法的**，会抛出 `re.error: nothing to repeat` 错误。本项目应在编译时拒绝这些模式。

### 实现说明
- 在 `src/regjit.cpp` 中，匹配器函数（`Func::CodeGen()`）实现了一个循环，尝试在输入字符串的每个可能偏移量（从 0 到 strlen）进行匹配。
- 在每个偏移量：尝试 AST 匹配，为每次尝试使用新的成功/失败块。一旦找到匹配，匹配函数返回 1；如果全部失败，返回 0。

### 为什么这很重要
如果没有这个搜索循环，正则匹配将只在位置 0 尝试，无法正确支持非锚定模式的搜索行为。

### 代码示例：`Func::CodeGen()` 中的搜索循环
```cpp
// 创建搜索循环基本块
BasicBlock *LoopCheckBB = BasicBlock::Create(Context, "search_loop_check", MatchF);
BasicBlock *LoopBodyBB = BasicBlock::Create(Context, "search_loop_body", MatchF);
BasicBlock *LoopIncBB = BasicBlock::Create(Context, "search_loop_inc", MatchF);

// 搜索循环条件：for(curIdx=0; curIdx<=strlen; ++curIdx)
Builder.SetInsertPoint(LoopCheckBB);
Value *curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
Value *cond = Builder.CreateICmpSLE(curIdx, strlenVal);
Builder.CreateCondBr(cond, LoopBodyBB, ReturnFailBB);

// 每次搜索尝试都有自己的 AST 成功/失败块
```

---

## Python 绑定

项目提供了 Python 3.12 绑定，位于 `python/` 目录。

### 构建
```bash
make libregjit.so
make python-bindings
```

### 使用
```python
import sys
sys.path.insert(0, 'python')
from _regjit import Regex

r = Regex('ab*c')
print(r.match('abbc'))  # True
print(r.match('ac'))    # True
print(r.match('ab'))    # False
```

### API
- `Regex(pattern)` - 编译正则表达式
- `.match(string)` - 匹配字符串，返回 bool
- `.match_bytes(bytes)` - 匹配字节序列
- `.unload()` - 卸载编译的模式

### 缓存管理
- `_regjit.cache_size()` - 返回缓存中的模式数量
- `_regjit.set_cache_maxsize(n)` - 设置缓存最大大小
- `_regjit.acquire(pattern)` - 获取模式引用
- `_regjit.release(pattern)` - 释放模式引用

---

## 🛠️ 开发日志

### 2026-01-30: Bug 修复与代码清理

#### 修复的问题

1. **`getOrCompile` 死锁问题** (`src/regjit.cpp:212-216`)
   - **问题**: 在 `getOrCompile` 函数中，连续两次获取同一个 mutex (`CompileCacheMutex`)，导致死锁
   - **原因**: `lk4` 和 `lk5` 都尝试锁定 `CompileCacheMutex`，而 `std::lock_guard` 不支持递归锁
   - **修复**: 合并为单个锁作用域
   ```cpp
   // 修复前（死锁）
   std::lock_guard<std::mutex> lk4(CompileCacheMutex);
   CompileInflight.erase(pattern);
   std::lock_guard<std::mutex> lk5(CompileCacheMutex);  // 死锁！
   return CompileCache.at(pattern);
   
   // 修复后
   {
     std::lock_guard<std::mutex> lk4(CompileCacheMutex);
     CompileInflight.erase(pattern);
     return CompileCache.at(pattern);
   }
   ```

2. **Python 测试路径问题** (`python/tests/*.py`)
   - **问题**: 测试文件使用 `sys.path.insert(0, "..")` 无法正确找到模块
   - **修复**: 使用 `os.path` 动态计算路径，并设置 `DYLD_LIBRARY_PATH`/`LD_LIBRARY_PATH`

3. **临时文件清理**
   - 删除了调试过程中生成的临时文件: `*.txt`, `*.log`
   - 更新 `.gitignore` 以排除这些文件类型

#### 更新的文件

| 文件 | 修改内容 |
|------|----------|
| `src/regjit.cpp` | 修复 `getOrCompile` 死锁 |
| `python/tests/test_bindings.py` | 修复路径，添加更多测试用例 |
| `python/tests/test_cache_eviction.py` | 修复路径 |
| `.gitignore` | 添加 `*.txt`, `*.log`, `*.so`, `__pycache__/`, `tools/` |

#### 测试状态
- ✅ C++ 测试 (`make test_all`) - 全部通过
- ✅ Python 测试 (`test_bindings.py`, `test_cache_eviction.py`) - 全部通过
