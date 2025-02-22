#  🚀  Regular Expression JIT Compiler
This JIT compiler demonstrates **over 60x speedup** compared to C++'s std::regex through LLVM-based optimizations.

Key optimizations:
1. **Native machine code generation** avoids interpretation overhead
2. **Pattern-specific optimization** eliminates generic regex costs
3. **LLVM pipeline** applies target-specific optimizations
4. **Zero-copy matching** reduces memory operations

## 🛠️ Prerequisites
- LLVM 19+ (with development headers)
- Modern C++17 compiler (GCC 10+/Clang 12+/MSVC 2019+)
- CMake 3.15+

## 📥 LLVM Installation

### Ubuntu/Debian
```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 19
sudo apt-get install llvm-19-dev libclang-19-dev
```

### macOS (Homebrew)
```bash
brew install llvm@19
echo 'export PATH="/opt/homebrew/opt/llvm@19/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

### Windows (vcpkg)
```powershell
vcpkg install llvm:x64-windows
```

## 🔧 Build Instructions
```bash
# Configure project
mkdir build && cd build
cmake .. -DLLVM_DIR=/path/to/llvm-config.cmake  # e.g. /usr/lib/llvm-13/lib/cmake/llvm

# Build executable
make -j$(nproc)

# Output binary: build/regjit
```

## 🧪 sample
The sample give a sample about how to use it.
```bash
# Run sample
make clean && make sample
./sample

# Expected output:
[Test Pattern] a+b*
- Test 'aaab' : ✅ Match
- Test 'abb'  : ❌ No match
- Test 'b'    : ✅ Match
```
## 🧪 test
```bash
# Run sample
make clean && make test
./test
./test 

Generated LLVM IR:
; ModuleID = 'my_module'
source_filename = "my_module"

define i32 @match(ptr %Arg0) {
entry:
  %0 = alloca i32, align 4
  store i32 0, ptr %0, align 4
  %1 = alloca i32, align 4
  store i32 0, ptr %1, align 4
  br label %repeat_check

TrueBlock:                                        ; preds = %repeat_exit
  %2 = load i32, ptr %0, align 4
  %3 = add i32 %2, 1
  %4 = call i32 @strlen(ptr %Arg0)
  %5 = icmp eq i32 %3, %4
  br i1 %5, label %real_true, label %FalseBlock

FalseBlock:                                       ; preds = %TrueBlock, %repeat_loop
  ret i32 0

repeat_check:                                     ; preds = %repeat_success, %entry
  %6 = load i32, ptr %1, align 4
  %7 = icmp slt i32 %6, 3
  br i1 %7, label %repeat_loop, label %repeat_exit

repeat_loop:                                      ; preds = %repeat_check
  %8 = load i32, ptr %0, align 4
  %9 = getelementptr i8, ptr %Arg0, i32 %8
  %10 = load i8, ptr %9, align 1
  %11 = icmp ne i8 %10, 99
  br i1 %11, label %FalseBlock, label %repeat_success

repeat_success:                                   ; preds = %repeat_loop
  %12 = load i32, ptr %0, align 4
  %13 = add i32 %12, 1
  store i32 %13, ptr %0, align 4
  %14 = add i32 %6, 1
  store i32 %14, ptr %1, align 4
  br label %repeat_check

repeat_exit:                                      ; preds = %repeat_check
  %15 = load i32, ptr %0, align 4
  %16 = sub i32 %15, 1
  store i32 %16, ptr %0, align 4
  br label %TrueBlock

real_true:                                        ; preds = %TrueBlock
  ret i32 1
}

declare i32 @strlen(ptr)

Program exited with code: 1

Program exited with code: 0

Program exited with code: 0

Program exited with code: 0
```

## 🧠 Architecture Overview
1. **LLVM JIT Core**  
   Uses ORC JIT API for dynamic code generation
   - Custom optimization pipeline
   - Architecture-specific code generation


## 📊 Performance Metrics
 **Over 60 times speedup**
```bash
./bench 

Testing a{1000} on 1000 chars:

Generated LLVM IR:
; ModuleID = 'my_module'
source_filename = "my_module"

define i32 @match(ptr %Arg0) {
entry:
  %0 = alloca i32, align 4
  store i32 0, ptr %0, align 4
  %1 = alloca i32, align 4
  store i32 0, ptr %1, align 4
  br label %repeat_check

TrueBlock:                                        ; preds = %repeat_exit
  %2 = load i32, ptr %0, align 4
  %3 = add i32 %2, 1
  %4 = call i32 @strlen(ptr %Arg0)
  %5 = icmp eq i32 %3, %4
  br i1 %5, label %real_true, label %FalseBlock

FalseBlock:                                       ; preds = %TrueBlock, %repeat_loop
  ret i32 0

repeat_check:                                     ; preds = %repeat_success, %entry
  %6 = load i32, ptr %1, align 4
  %7 = icmp slt i32 %6, 1000
  br i1 %7, label %repeat_loop, label %repeat_exit

repeat_loop:                                      ; preds = %repeat_check
  %8 = load i32, ptr %0, align 4
  %9 = getelementptr i8, ptr %Arg0, i32 %8
  %10 = load i8, ptr %9, align 1
  %11 = icmp ne i8 %10, 97
  br i1 %11, label %FalseBlock, label %repeat_success

repeat_success:                                   ; preds = %repeat_loop
  %12 = load i32, ptr %0, align 4
  %13 = add i32 %12, 1
  store i32 %13, ptr %0, align 4
  %14 = add i32 %6, 1
  store i32 %14, ptr %1, align 4
  br label %repeat_check

repeat_exit:                                      ; preds = %repeat_check
  %15 = load i32, ptr %0, align 4
  %16 = sub i32 %15, 1
  store i32 %16, ptr %0, align 4
  br label %TrueBlock

real_true:                                        ; preds = %TrueBlock
  ret i32 1
}

declare i32 @strlen(ptr)
JIT time (1000): 375 ns/iter
std::regex time (1000): 26185 ns/iter (Speedup: 69x)
```
