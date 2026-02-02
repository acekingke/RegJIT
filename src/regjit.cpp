#include "regjit.h"
#include <iostream>
#include <stdexcept>
#include "regjit_capi.h"
#include <future>
#include <thread>
#include <chrono>
#include "llvm/IR/Verifier.h"

// ARM NEON SIMD support
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define HAS_NEON 1
#else
#define HAS_NEON 0
#endif

// Debug printing macro: enable by defining REGJIT_DEBUG (e.g. -DREGJIT_DEBUG)
#ifdef REGJIT_DEBUG
#define RJDBG(x) x
#else
#define RJDBG(x) do {} while(0)
#endif

  ExitOnError ExitOnErr;
  // Global fallback context/builder used when not compiling. Per-compile code
  // will create its own LLVMContext and IRBuilder and temporarily point
  // ContextPtr/BuilderPtr at them.
  LLVMContext GlobalContext;
  IRBuilder<> GlobalBuilder(GlobalContext);
  // Pointers used by CodeGen; default to global instances.
  LLVMContext* ContextPtr = &GlobalContext;
  IRBuilder<>* BuilderPtr = &GlobalBuilder;

  // Convenience macros so existing CodeGen code can use `Context` and
  // `Builder` names without changing every reference.
  #undef Context
  #define Context (*ContextPtr)
  #undef Builder
  #define Builder (*BuilderPtr)
  std::unique_ptr<Module>ThisModule = nullptr;
  std::unique_ptr<llvm::orc::LLJIT> JIT;
  llvm::orc::ResourceTrackerSP RT;
  // Per-compile owned context/builder (kept alive during compilation).
  std::unique_ptr<LLVMContext> OwnedCompileContext;
  std::unique_ptr<IRBuilder<>> OwnedCompileBuilder;
  Function *MatchF;
    // Default to empty to indicate "no explicit name chosen".
    // Previously this was the literal "match" which made the check for
    // whether to generate a unique name brittle. Use empty() semantics
    // and treat any non-empty value as an explicit name.
    std::string FunctionName("");
   // Compilation cache and helpers
    std::unordered_map<std::string, CompiledEntry> CompileCache;
    std::mutex CompileCacheMutex;
    // In-flight compile coordination: pattern -> promise/shared_future
    std::unordered_map<std::string, InflightCompile> CompileInflight;
   std::atomic<uint64_t> GlobalFnId{0};
   size_t CacheMaxSize = 64; // default max entries
   std::list<std::string> CacheLRUList;
  const std::string FunArgName("Arg0");
  const std::string TrueBlockName("TrueBlock");
  const std::string FalseBlockName("FalseBlock");
  Value *Index;
  Value *Arg0;
  Value *StrLenAlloca = nullptr;
  

void Initialize() {
   if (!JIT) {
     InitializeNativeTarget();
     InitializeNativeTargetAsmPrinter();
     JIT = ExitOnErr(LLJITBuilder().create());
   }
  // Allow the JIT to resolve symbols from the host process (e.g. libc's strlen)
  JIT->getMainJITDylib().addGenerator(
      cantFail(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          JIT->getDataLayout().getGlobalPrefix())));

  // We compute string length inline inside Func::CodeGen to avoid depending
  // on host libc symbol resolution (e.g. renamed variants like "strlen.1").
  // Keep DynamicLibrarySearchGenerator in case other external symbols are
  // required by generated code.
  
  // Do not create a global ThisModule here. Modules must be created per-compile
  // in the compile path so they are backed by the correct LLVMContext.

}

// Runtime trace helper called from generated code. Keep C linkage so the
// JIT can resolve the symbol via DynamicLibrarySearchGenerator.
extern "C" void regjit_trace(const char* tag, int idx, int cnt) {
    RJDBG(fprintf(stderr, "regjit_trace: %s idx=%d cnt=%d\n", tag, idx, cnt));
}

// Boyer-Moore-Horspool string search implementation with memchr optimization.
// Returns pointer to first occurrence of needle in haystack, or nullptr if not found.
// This combines BMH bad-character shifts with memchr SIMD acceleration.
extern "C" const char* regjit_bmh_search(const char* haystack, size_t haystackLen,
                                          const char* needle, size_t needleLen) {
    if (needleLen == 0) return haystack;
    if (needleLen > haystackLen) return nullptr;
    
    // For single character, just use memchr (SIMD optimized)
    if (needleLen == 1) {
        return (const char*)memchr(haystack, needle[0], haystackLen);
    }
    
    // For short needles (2-3 chars), use memchr + verify approach
    // This is typically faster than building the shift table
    if (needleLen <= 3) {
        const char* p = haystack;
        const char* end = haystack + haystackLen - needleLen + 1;
        const char firstChar = needle[0];
        
        while (p < end) {
            p = (const char*)memchr(p, firstChar, end - p);
            if (!p) return nullptr;
            if (memcmp(p, needle, needleLen) == 0) return p;
            p++;
        }
        return nullptr;
    }
    
    // For longer needles, use full BMH with memchr for initial search
    const char firstChar = needle[0];
    const char lastChar = needle[needleLen - 1];
    const size_t lastIdx = needleLen - 1;
    
    // Build the bad character shift table (only for chars in needle)
    // Using a simple array is faster than unordered_map
    size_t shift[256];
    for (int i = 0; i < 256; i++) {
        shift[i] = needleLen;
    }
    for (size_t i = 0; i < needleLen - 1; i++) {
        shift[(unsigned char)needle[i]] = needleLen - 1 - i;
    }
    
    // Use memchr to find first char, then verify with BMH-style shifts
    const char* p = haystack;
    const char* end = haystack + haystackLen;
    
    while (p <= end - needleLen) {
        // Use memchr to find first character (SIMD accelerated)
        p = (const char*)memchr(p, firstChar, end - p - lastIdx);
        if (!p) return nullptr;
        
        // Check last char quickly
        if (p[lastIdx] == lastChar) {
            // Full comparison (skip first and last which we already checked)
            if (needleLen == 2 || memcmp(p + 1, needle + 1, needleLen - 2) == 0) {
                return p;  // Found!
            }
        }
        
        // Move forward - use BMH shift if beneficial, otherwise just +1
        size_t bmhShift = shift[(unsigned char)p[lastIdx]];
        p += (bmhShift > 1) ? bmhShift : 1;
    }
    
    return nullptr;  // Not found
}

// Count consecutive occurrences of a character starting from pos.
// Uses SIMD when available for faster scanning.
// Returns the count of consecutive matching characters.
extern "C" size_t regjit_count_char(const char* str, size_t len, char target) {
    if (len == 0) return 0;
    
#if HAS_NEON
    // ARM NEON optimized path (Apple Silicon, ARM64)
    size_t count = 0;
    uint8x16_t vtarget = vdupq_n_u8((uint8_t)target);
    
    // Process 16 bytes at a time with NEON
    while (count + 16 <= len) {
        uint8x16_t vdata = vld1q_u8((const uint8_t*)(str + count));
        uint8x16_t vcmp = vceqq_u8(vdata, vtarget);
        
        // Check if all 16 bytes match using 64-bit lane comparison
        uint64x2_t vcmp64 = vreinterpretq_u64_u8(vcmp);
        uint64_t low = vgetq_lane_u64(vcmp64, 0);
        uint64_t high = vgetq_lane_u64(vcmp64, 1);
        
        if (low == 0xFFFFFFFFFFFFFFFFULL && high == 0xFFFFFFFFFFFFFFFFULL) {
            count += 16;
        } else {
            // Some bytes don't match - find the first non-match
            for (int i = 0; i < 16 && count + i < len; i++) {
                if (str[count + i] != target) return count + i;
            }
            count += 16;
        }
    }
    
    // Handle remaining bytes
    while (count < len && str[count] == target) {
        count++;
    }
    return count;
    
#else
    // Generic optimized path - unroll loop for better pipelining
    size_t count = 0;
    
    // Process 8 bytes at a time
    while (count + 8 <= len) {
        if (str[count] != target) return count;
        if (str[count + 1] != target) return count + 1;
        if (str[count + 2] != target) return count + 2;
        if (str[count + 3] != target) return count + 3;
        if (str[count + 4] != target) return count + 4;
        if (str[count + 5] != target) return count + 5;
        if (str[count + 6] != target) return count + 6;
        if (str[count + 7] != target) return count + 7;
        count += 8;
    }
    
    // Handle remaining bytes
    while (count < len && str[count] == target) {
        count++;
    }
    
    return count;
#endif
}

// NOTE: previous attempts to defensively create new blocks when the current
// insert block already had a terminator caused verifier failures because some
// of those helper-created blocks remained empty (no terminator). Instead of
// auto-creating blocks we perform explicit checks at emission sites and only
// emit follow-on instructions when the current block has no terminator.

void ensureJITInitialized() {
  if (!JIT) {
    Initialize();
  }
}

// Ensure the given basic block has a terminator. If it doesn't, insert an
// unconditional branch to the provided target. This avoids creating empty
// blocks without terminators which causes verifier failures.
static void ensureBlockHasTerminator(BasicBlock* B, BasicBlock* target) {
    if (!B) return;
    if (B->getTerminator() == nullptr) {
        IRBuilder<> Tmp(B);
        Tmp.SetInsertPoint(B);
        Tmp.CreateBr(target);
    }
}

// Reset compile-time state after a failed compile.
static void resetCompileState() {
    ThisModule.reset();
    OwnedCompileBuilder.reset();
    OwnedCompileContext.reset();
    ContextPtr = &GlobalContext;
    BuilderPtr = &GlobalBuilder;
    FunctionName.clear();
}

// compile-or-get with cache. This uses CompileRegex which generates IR into
// the global ThisModule and calls Compile() to add it to the JIT. We hold
// CompileCacheMutex during compilation to avoid races and RT being overwritten.
CompiledEntry getOrCompile(const std::string &pattern) {
  // Fast-path: return if already cached
  {
    std::lock_guard<std::mutex> lk(CompileCacheMutex);
    auto it = CompileCache.find(pattern);
    if (it != CompileCache.end()) {
      fprintf(stderr, "getOrCompile: cache HIT for pattern='%s' fn='%s'\n", pattern.c_str(), it->second.FnName.c_str());
      return it->second;
    }
  }

  // Coordinate concurrent compilations per-pattern using promises/futures.
  // If another thread is compiling the same pattern, wait for it.
  {
    std::unique_lock<std::mutex> lk(CompileCacheMutex);
    auto it = CompileCache.find(pattern);
    if (it != CompileCache.end()) return it->second;

    auto inflIt = CompileInflight.find(pattern);
    if (inflIt != CompileInflight.end()) {
      auto fut = inflIt->second.fut;
      // unlock while waiting
      fprintf(stderr, "getOrCompile: waiting for inflight compile for pattern='%s'\n", pattern.c_str());
      lk.unlock();
      bool ok = fut->get();
      if (!ok) throw std::runtime_error("concurrent compile failed");
      std::lock_guard<std::mutex> lk2(CompileCacheMutex);
      fprintf(stderr, "getOrCompile: inflight compile finished for pattern='%s'\n", pattern.c_str());
      return CompileCache.at(pattern);
    }

    // No inflight compile: become the compiler
    auto prom = std::make_shared<std::promise<bool>>();
    auto sf = std::make_shared<std::shared_future<bool>>(prom->get_future().share());
    CompileInflight.emplace(pattern, InflightCompile{prom, sf});
    // release lock while compiling
    lk.unlock();

    // Now perform compilation (same logic as before)
    try {
      ensureJITInitialized();

      uint64_t id = GlobalFnId.fetch_add(1);
      std::hash<std::string> hasher;
      auto h = hasher(pattern);
      FunctionName = "regjit_match_" + std::to_string(h) + "_" + std::to_string(id);
      RJDBG(fprintf(stderr, "getOrCompile: compiling pattern='%s' -> FunctionName='%s'\n", pattern.c_str(), FunctionName.c_str()));

      // create fresh per-compile LLVMContext and IRBuilder, and set pointers so
      // CodeGen uses them via the Context/Builder macros
      OwnedCompileContext = std::make_unique<LLVMContext>();
      OwnedCompileBuilder = std::make_unique<IRBuilder<>>(*OwnedCompileContext);
      // point the Context/Builder macros at the per-compile instances
      ContextPtr = OwnedCompileContext.get();
      BuilderPtr = OwnedCompileBuilder.get();

      // create fresh module for this compile within the per-compile context
      ThisModule = std::make_unique<Module>("module_" + std::to_string(id), Context);
      ThisModule->setDataLayout(JIT->getDataLayout());

      // Call existing CompileRegex which will fill ThisModule and call Compile()
      if (!CompileRegex(pattern)) {
        resetCompileState();
        // signal failure and clean inflight
        prom->set_value(false);
        std::lock_guard<std::mutex> lk2(CompileCacheMutex);
        CompileInflight.erase(pattern);
        throw std::runtime_error("compile failed");
      }

      // After Compile(), RT holds the ResourceTracker used for this module
      auto Sym = ExitOnErr(JIT->lookup(FunctionName));
      uint64_t addr = Sym.getValue();

      CompiledEntry e;
      e.Addr = addr;
      e.RT = RT; // RT set by Compile()
      e.FnName = FunctionName;
      e.refCount = 1;

      // insert into LRU front
      {
        std::lock_guard<std::mutex> lk3(CompileCacheMutex);
        CacheLRUList.push_front(pattern);
        e.lruIt = CacheLRUList.begin();
        CompileCache.emplace(pattern, std::move(e));
      }

      // Evict if needed
      evictIfNeeded();

      // signal success and remove inflight entry
      prom->set_value(true);
      {
        std::lock_guard<std::mutex> lk4(CompileCacheMutex);
        CompileInflight.erase(pattern);
        return CompileCache.at(pattern);
      }
    } catch (...) {
      try {
        prom->set_value(false);
      } catch (...) {}
      std::lock_guard<std::mutex> lkErr(CompileCacheMutex);
      CompileInflight.erase(pattern);
      throw;
    }
  }
}

// Acquire API: increment refCount for pattern, compiling if necessary.
int regjit_acquire(const char* cpattern, char** err_msg) {
  if (!cpattern) {
    if (err_msg) *err_msg = strdup("null pattern");
    return 0;
  }
  std::string pattern(cpattern);
  try {
    std::lock_guard<std::mutex> lk(CompileCacheMutex);
    auto it = CompileCache.find(pattern);
    if (it != CompileCache.end()) {
      it->second.refCount++;
      // move to front of LRU
      CacheLRUList.erase(it->second.lruIt);
      CacheLRUList.push_front(pattern);
      it->second.lruIt = CacheLRUList.begin();
      return 1;
    }
  } catch (...) {
    if (err_msg) *err_msg = strdup("internal error");
    return 0;
  }

  // Not found -> compile (outside lock to allow getOrCompile locking)
  try {
    auto entry = getOrCompile(pattern);
    (void)entry;
    return 1;
  } catch (const std::exception &e) {
    if (err_msg) *err_msg = strdup(e.what());
    return 0;
  }
}

void regjit_release(const char* cpattern) {
  if (!cpattern) return;
  releasePattern(std::string(cpattern));
}

// Cache helpers (C API)
size_t regjit_cache_size() {
  std::lock_guard<std::mutex> lk(CompileCacheMutex);
  return CompileCache.size();
}

void regjit_set_cache_maxsize(size_t n) {
  std::lock_guard<std::mutex> lk(CompileCacheMutex);
  CacheMaxSize = n;
  evictIfNeeded();
}

// Evict entries until cache size <= CacheMaxSize. Only evict entries with refCount == 0.
void evictIfNeeded() {
  while (CompileCache.size() > CacheMaxSize) {
    if (CacheLRUList.empty()) break;
    std::string victim = CacheLRUList.back();
    auto it = CompileCache.find(victim);
    if (it == CompileCache.end()) {
      CacheLRUList.pop_back();
      continue;
    }
    if (it->second.refCount == 0) {
      // remove module via ResourceTracker
      if (it->second.RT) {
        ExitOnErr(it->second.RT->remove());
      }
      // erase from cache and LRU
      CacheLRUList.pop_back();
      CompileCache.erase(it);
    } else {
      // cannot evict currently referenced item; stop
      break;
    }
  }
}

// release a pattern reference (decrement refCount and possibly evict)
void releasePattern(const std::string &pattern) {
  std::lock_guard<std::mutex> lk(CompileCacheMutex);
  auto it = CompileCache.find(pattern);
  if (it == CompileCache.end()) return;
  if (it->second.refCount > 0) it->second.refCount--;
  // move to front of LRU when used? keep as is
  // if now zero and cache oversized, evict
  if (it->second.refCount == 0) {
    evictIfNeeded();
  }
}

int ExecutePattern(const std::string& pattern, const char* input) {
  // compile the pattern (CompileRegex generates function named 'match')
  if (!CompileRegex(pattern)) return -1;
  return Execute(input);
}

void unloadPattern(const std::string& pattern) {
  std::lock_guard<std::mutex> lk(CompileCacheMutex);
  auto it = CompileCache.find(pattern);
  if (it == CompileCache.end()) return;
  if (it->second.RT) {
    ExitOnErr(it->second.RT->remove());
  }
  CompileCache.erase(it);
}

// C API implementations
int regjit_compile(const char* pattern, char** err_msg) {
  try {
    bool ok = CompileRegex(std::string(pattern));
    if (!ok) {
      if (err_msg) *err_msg = strdup("compile failed");
      return 0;
    }
    return 1;
  } catch (const std::exception &e) {
    if (err_msg) *err_msg = strdup(e.what());
    return 0;
  }
}

int regjit_match(const char* pattern, const char* buf, size_t len) {
  // Our generated function expects null-terminated C string; ensure termination
  // For performance we avoid copying if buf[len]==0; otherwise copy to temp buffer
  bool need_copy = false;
  if (len == 0 || buf[len-1] != '\0') need_copy = true;
  std::string tmp;
  const char* cstr = nullptr;
  if (need_copy) {
    tmp.assign(buf, len);
    tmp.push_back('\0');
    cstr = tmp.c_str();
  } else {
    cstr = buf;
  }
   if (!pattern) return -1;

   // Acquire the compiled pattern to ensure it isn't evicted while executing
   char* err = nullptr;
   if (!regjit_acquire(pattern, &err)) {
     if (err) free(err);
     return -1;
   }

   // Find the compiled entry and call its function pointer (Addr)
   uint64_t addr = 0;
   {
     std::lock_guard<std::mutex> lk(CompileCacheMutex);
     auto it = CompileCache.find(std::string(pattern));
     if (it != CompileCache.end()) {
       addr = it->second.Addr;
     }
   }

   int result = -1;
   if (addr != 0) {
     auto Func = (int (*)(const char*))(uintptr_t)addr;
     result = Func(cstr);
   }

   // Release the acquired ref
   regjit_release(pattern);

   return result;
}

void regjit_unload(const char* pattern) {
  if (!pattern) return;
  unloadPattern(std::string(pattern));
}

// Add optimization passes
void OptimizeModule(Module& M) {
    // Create pass managers
    PassBuilder PB;
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;
    
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    // Configure optimization level to O2
    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
    
    // Run optimization
    MPM.run(M, MAM);
}

// Modified Compile function
void Compile() {
  // Set data layout for the target

  // Diagnostic IR dump: only print when REGJIT_DEBUG is enabled
  RJDBG({ outs() << "\nGenerated LLVM IR:\n"; ThisModule->print(outs(), nullptr); });

  // Diagnostic info (only when debugging)
  RJDBG({
    if (!OwnedCompileContext) {
      errs() << "Compile(): OwnedCompileContext is NULL\n";
    } else {
      errs() << "Compile(): OwnedCompileContext addr=" << (void*)OwnedCompileContext.get() << "\n";
    }
    errs() << "Compile(): ThisModule context=" << (void*)&ThisModule->getContext() << "\n";
  });

  // Run optimization pipeline on the module built in the per-compile
  // LLVMContext. OptimizeModule expects the Module to be backed by the
  // same LLVMContext used to generate the IR, so we must call it while
  // OwnedCompileContext is still alive.
  // Dump the module to a temporary file for debugging so we can
  // inspect IR that triggers optimizer crashes.
  RJDBG({
    // Create a unique dump path using timestamp + thread id to avoid needing
    // platform-specific getpid(). This avoids including <unistd.h> here.
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::hash<std::thread::id> hasher_tid;
    auto tid_hash = hasher_tid(std::this_thread::get_id());
    std::string dumpPath = "/tmp/regjit_ir_" + std::to_string(now) + "_" + std::to_string(tid_hash) + "_" + FunctionName + ".ll";
    std::error_code EC;
    llvm::raw_fd_ostream ofs(dumpPath, EC, llvm::sys::fs::OF_Text);
    if (!EC) {
      ThisModule->print(ofs, nullptr);
      ofs.close();
    } else {
      errs() << "Failed to open dump file: " << EC.message() << "\n";
    }
  });
  // Verify IR before running optimizer to avoid crashes in LLVM when IR is
  // malformed. If verification fails, write the dump and abort compilation
  // so we can fix the generator instead of crashing inside the optimizer.
  if (llvm::verifyModule(*ThisModule, &errs())) {
    errs() << "Module verification failed; aborting Compile() to avoid optimizer crash.\n";
    throw std::runtime_error("Module verification failed");
  }

  OptimizeModule(*ThisModule);

  // Add module to JIT. Move the per-compile LLVMContext (OwnedCompileContext)
  // into a ThreadSafeContext so the JIT owns it and it remains valid while
  // the module is active.
  RT = JIT->getMainJITDylib().createResourceTracker();

  // OwnedCompileBuilder may reference the OwnedCompileContext; drop the
  // builder before transferring ownership to the JIT.
  OwnedCompileBuilder.reset();

  // Transfer ownership of the compile-time LLVMContext into ThreadSafeContext
  RJDBG(fprintf(stderr, "Compile(): OwnedCompileContext=%p ThisModule_ctx=%p\n", (void*)OwnedCompileContext.get(), (void*)&ThisModule->getContext()));
  ThreadSafeContext SafeCtx(std::move(OwnedCompileContext));

  ExitOnErr(JIT->addIRModule(RT, ThreadSafeModule(std::move(ThisModule), SafeCtx)));

  // restore builder/context pointers to global defaults
  ContextPtr = &GlobalContext;
  BuilderPtr = &GlobalBuilder;
}
void CleanUp() {
  if (RT) {
    ExitOnErr(RT->remove());
    RT = nullptr;
  }
  // Note: We don't delete JIT here as it might be reused
  // JIT resources will be cleaned up when the JIT object is destroyed
}
int Execute(const char* input) {
  // Use the last generated function name if available; fall back to legacy
  // "match" for compatibility with older code paths.
  std::string lookupName = FunctionName.empty() ? "match" : FunctionName;
  RJDBG(fprintf(stderr, "Execute(): FunctionName='%s' lookupName='%s'\n", FunctionName.c_str(), lookupName.c_str()));
  auto MatchSym = ExitOnErr(JIT->lookup(lookupName));
  auto Func = (int (*)(const char*))MatchSym.getValue();

  int ResultCode = Func(input);
  outs() << "\nProgram exited with code: " << ResultCode << "\n";
  return ResultCode;
}

//
// Anchor/Quantifier Search Mode Note:
// PCRE, std::regex, and RE2 all require that anchors (e.g. ^, $, \b) with quantifiers (e.g. ^*, $+) are matched by attempting the regex at every possible offset in the input string.
// This search loop (and the logic below) is essential for correct zero-width anchor + quantifier compatibility. DO NOT REMOVE/REFRACTOR this loop unless you re-run all anchor/quant edge tests against PCRE/RE2.
//
Value* Func::CodeGen() {
    FunctionType *matchFuncType = FunctionType::get(
        Builder.getInt32Ty(), 
        {PointerType::get(Builder.getInt8Ty(), 0)},
        false
    );
    MatchF = Function::Create(
        matchFuncType, Function::ExternalLinkage, FunctionName, ThisModule.get());

    Arg0 = MatchF->arg_begin();
    Arg0->setName(FunArgName);

    // Create entry block and index variable
    BasicBlock *EntryBB = BasicBlock::Create(Context, "entry", MatchF);
    Builder.SetInsertPoint(EntryBB);
    Index = Builder.CreateAlloca(Builder.getInt32Ty());
    Builder.CreateStore(ConstantInt::get(Context, APInt(32, 0)), Index);
    
    // Compute string length using libc strlen (with direct function pointer)
    // This is much faster than the inline loop for long strings
    StrLenAlloca = Builder.CreateAlloca(Builder.getInt32Ty());
    
    Type* i8ptrTy = PointerType::get(Builder.getInt8Ty(), 0);
    Type* sizeTy = Builder.getInt64Ty();
    
    // Get strlen function pointer directly
    FunctionType* strlenFnTy = FunctionType::get(sizeTy, {i8ptrTy}, false);
    auto strlenAddr = reinterpret_cast<uintptr_t>(&strlen);
    Value* strlenPtrInt = ConstantInt::get(Builder.getInt64Ty(), strlenAddr);
    Value* strlenPtr = Builder.CreateIntToPtr(strlenPtrInt, PointerType::get(strlenFnTy, 0));
    
    // Call strlen and truncate to i32
    Value* strlenResult = Builder.CreateCall(strlenFnTy, strlenPtr, {Arg0});
    Value* strlenVal32 = Builder.CreateTrunc(strlenResult, Builder.getInt32Ty());
    Builder.CreateStore(strlenVal32, StrLenAlloca);
    
    BasicBlock *PostStrlenBB = BasicBlock::Create(Context, "post_strlen", MatchF);
    Builder.CreateBr(PostStrlenBB);
    
    // Create return blocks (reusable)
    BasicBlock *ReturnFailBB = BasicBlock::Create(Context, "return_fail", MatchF);
    BasicBlock *ReturnSuccessBB = BasicBlock::Create(Context, "return_success", MatchF);

    // Optimization: if the AST is anchored at start and there are no
    // zero-width repeats (e.g. '^' not repeated), we can skip the search
    // loop and attempt the pattern only at index 0. This is safe and
    // preserves semantics while avoiding unnecessary scanning.
    if (Body->isAnchoredAtStart() && !Body->containsZeroWidthRepeat()) {
        // This is the OPTIMIZATION PATH (no search loop)
        
        // Connect post_strlen directly to this path's entry.
        Builder.SetInsertPoint(PostStrlenBB);
        BasicBlock *SingleAttemptBB = BasicBlock::Create(Context, "single_attempt", MatchF);
        Builder.CreateBr(SingleAttemptBB);
        Builder.SetInsertPoint(SingleAttemptBB);
        
        // Ensure index is 0 and emit a single attempt
        Builder.CreateStore(ConstantInt::get(Context, APInt(32, 0)), Index);
        // Trace attempt at idx=0
        {
          Type* i8ptrTy = PointerType::get(Builder.getInt8Ty(), 0);
          FunctionCallee traceFn = ThisModule->getOrInsertFunction("regjit_trace",
            FunctionType::get(Builder.getVoidTy(), {i8ptrTy, Builder.getInt32Ty(), Builder.getInt32Ty()}, false));
          Value* tag = Builder.CreateGlobalStringPtr("attempt");
          Builder.CreateCall(traceFn, {tag, ConstantInt::get(Builder.getInt32Ty(), 0), ConstantInt::get(Builder.getInt32Ty(), 0)});
        }


        Body->SetFailBlock(ReturnFailBB);
        Body->SetSuccessBlock(ReturnSuccessBB);
        Body->CodeGen();

        // The Body->CodeGen() call will have already generated terminators
        // that branch to either ReturnSuccessBB or ReturnFailBB. We don't need to
        // add more branches here. The blocks are already properly terminated.

    } else {
        // This is the SEARCH LOOP PATH
        Builder.SetInsertPoint(PostStrlenBB);
        Value* strlenVal = Builder.CreateLoad(Builder.getInt32Ty(), StrLenAlloca);

        // Check optimization opportunities
        std::string literalPrefix = Body->getLiteralPrefix();
        bool isPureLiteral = Body->isPureLiteral();
        int firstLiteralChar = Body->getFirstLiteralChar();
        
        Type* i8ptrTy = PointerType::get(Builder.getInt8Ty(), 0);
        Type* sizeTy = Builder.getInt64Ty();

        if (isPureLiteral && literalPrefix.length() > 0) {
            // === BOYER-MOORE-HORSPOOL OPTIMIZATION PATH ===
            // For pure literal patterns, use BMH to find the entire string at once
            // This is faster than memmem on macOS for longer patterns
            
            RJDBG(std::cerr << "Using BMH optimization for literal: " << literalPrefix << "\n");
            
            // Get the function type for regjit_bmh_search
            FunctionType* bmhFnTy = FunctionType::get(i8ptrTy, {i8ptrTy, sizeTy, i8ptrTy, sizeTy}, false);
            
            // Create function pointer by embedding the address directly
            // This avoids symbol lookup overhead at runtime
            auto bmhAddr = reinterpret_cast<uintptr_t>(&regjit_bmh_search);
            Value* bmhPtrInt = ConstantInt::get(Builder.getInt64Ty(), bmhAddr);
            Value* bmhPtr = Builder.CreateIntToPtr(bmhPtrInt, PointerType::get(bmhFnTy, 0));
            
            // Create a global string constant for the needle
            Value* needlePtr = Builder.CreateGlobalStringPtr(literalPrefix, "needle");
            Value* needleLen = ConstantInt::get(sizeTy, literalPrefix.length());
            
            // Call regjit_bmh_search(Arg0, strlen, needle, needlelen) via function pointer
            Value* haystackLen = Builder.CreateZExt(strlenVal, sizeTy);
            Value* foundPtr = Builder.CreateCall(bmhFnTy, bmhPtr, {Arg0, haystackLen, needlePtr, needleLen});
            
            // Check if BMH found anything (returns nullptr if not found)
            Value* isNull = Builder.CreateICmpEQ(foundPtr, ConstantPointerNull::get(cast<PointerType>(i8ptrTy)));
            Builder.CreateCondBr(isNull, ReturnFailBB, ReturnSuccessBB);
            
        } else if (firstLiteralChar >= 0) {
            // === MEMCHR OPTIMIZATION PATH ===
            // Use memchr to quickly find the next occurrence of the first literal character
            // instead of checking every position sequentially.
            
            RJDBG(std::cerr << "Using memchr optimization for first char: " << (char)firstLiteralChar << "\n");
            
            // Declare memchr: void* memchr(const void* s, int c, size_t n)
            FunctionCallee memchrFn = ThisModule->getOrInsertFunction("memchr",
                FunctionType::get(i8ptrTy, {i8ptrTy, Builder.getInt32Ty(), sizeTy}, false));
            
            // Create memchr search blocks
            BasicBlock *MemchrSearchBB = BasicBlock::Create(Context, "memchr_search", MatchF);
            BasicBlock *MemchrFoundBB = BasicBlock::Create(Context, "memchr_found", MatchF);
            BasicBlock *LoopBodyBB = BasicBlock::Create(Context, "search_loop_body", MatchF);
            
            Builder.CreateBr(MemchrSearchBB);
            
            // Memchr search: find next occurrence of first char starting from current index
            Builder.SetInsertPoint(MemchrSearchBB);
            Value *curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
            Value *searchPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, {curIdx});
            // remaining = strlen - curIdx
            Value *remaining = Builder.CreateSub(strlenVal, curIdx);
            Value *remainingSize = Builder.CreateZExt(remaining, sizeTy);
            
            // Call memchr(searchPtr, firstChar, remaining)
            Value *firstCharVal = ConstantInt::get(Builder.getInt32Ty(), firstLiteralChar);
            Value *foundPtr = Builder.CreateCall(memchrFn, {searchPtr, firstCharVal, remainingSize});
            
            // Check if memchr found anything (returns null if not found)
            Value *isNull = Builder.CreateICmpEQ(foundPtr, ConstantPointerNull::get(cast<PointerType>(i8ptrTy)));
            Builder.CreateCondBr(isNull, ReturnFailBB, MemchrFoundBB);
            
            // Memchr found: calculate the new index
            Builder.SetInsertPoint(MemchrFoundBB);
            // newIdx = foundPtr - Arg0 (pointer subtraction)
            Value *foundPtrInt = Builder.CreatePtrToInt(foundPtr, sizeTy);
            Value *arg0PtrInt = Builder.CreatePtrToInt(Arg0, sizeTy);
            Value *newIdxLong = Builder.CreateSub(foundPtrInt, arg0PtrInt);
            Value *newIdx = Builder.CreateTrunc(newIdxLong, Builder.getInt32Ty());
            Builder.CreateStore(newIdx, Index);
            Builder.CreateBr(LoopBodyBB);
            
            // Loop body: try to match the full pattern at this position
            Builder.SetInsertPoint(LoopBodyBB);
            BasicBlock *TrySuccess = BasicBlock::Create(Context, "try_success", MatchF);
            BasicBlock *TryFail = BasicBlock::Create(Context, "try_fail", MatchF);
            
            Value *curIdx_search = Builder.CreateLoad(Builder.getInt32Ty(), Index);
            Builder.CreateStore(curIdx_search, Index);
            
            Body->SetFailBlock(TryFail);
            Body->SetSuccessBlock(TrySuccess);
            Body->CodeGen();
            
            // If body succeeds, return success immediately
            Builder.SetInsertPoint(TrySuccess);
            Builder.CreateBr(ReturnSuccessBB);
            
            // If body fails, increment index and search again with memchr
            Builder.SetInsertPoint(TryFail);
            // We need to skip past the current position to avoid infinite loop
            Value* nextIdx = Builder.CreateAdd(curIdx_search, ConstantInt::get(Context, APInt(32, 1)));
            Builder.CreateStore(nextIdx, Index);
            Builder.CreateBr(MemchrSearchBB);
            
        } else {
            // === STANDARD SEARCH LOOP ===
            
            // Check if pattern has required characters for memchr-accelerated search
            std::set<char> requiredChars = Body->getRequiredChars();
            
            if (!requiredChars.empty()) {
                // === MEMCHR-ACCELERATED SEARCH LOOP ===
                // Use memchr to find positions where required char exists,
                // then limit search to only those candidate regions
                char filterChar = *requiredChars.begin();
                RJDBG(std::cerr << "Using memchr-accelerated search for required char: '" << filterChar << "'\n");
                
                // Declare memchr
                FunctionCallee memchrFn = ThisModule->getOrInsertFunction("memchr",
                    FunctionType::get(i8ptrTy, {i8ptrTy, Builder.getInt32Ty(), sizeTy}, false));
                Value* filterCharVal = ConstantInt::get(Builder.getInt32Ty(), static_cast<unsigned char>(filterChar));
                
                // Create blocks for the accelerated search
                BasicBlock *MemchrSearchBB = BasicBlock::Create(Context, "memchr_search", MatchF);
                BasicBlock *MemchrFoundBB = BasicBlock::Create(Context, "memchr_found", MatchF);
                BasicBlock *RangeLoopCheckBB = BasicBlock::Create(Context, "range_loop_check", MatchF);
                BasicBlock *RangeLoopBodyBB = BasicBlock::Create(Context, "range_loop_body", MatchF);
                BasicBlock *NextMemchrBB = BasicBlock::Create(Context, "next_memchr", MatchF);
                
                // Store the "range end" - we'll search positions 0..foundPos for each memchr hit
                AllocaInst* RangeEndAlloca = Builder.CreateAlloca(Builder.getInt32Ty(), nullptr, "range_end");
                AllocaInst* RangeStartAlloca = Builder.CreateAlloca(Builder.getInt32Ty(), nullptr, "range_start");
                AllocaInst* MemchrPosAlloca = Builder.CreateAlloca(Builder.getInt32Ty(), nullptr, "memchr_pos");
                
                // Initialize: first memchr search starts at position 0
                Builder.CreateStore(ConstantInt::get(Builder.getInt32Ty(), 0), RangeStartAlloca);
                Builder.CreateStore(ConstantInt::get(Builder.getInt32Ty(), 0), MemchrPosAlloca);
                Builder.CreateBr(MemchrSearchBB);
                
                // === MEMCHR SEARCH BLOCK ===
                // Find next occurrence of required char
                Builder.SetInsertPoint(MemchrSearchBB);
                Value* memchrStartPos = Builder.CreateLoad(Builder.getInt32Ty(), MemchrPosAlloca);
                Value* searchPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, {memchrStartPos});
                Value* remaining = Builder.CreateSub(strlenVal, memchrStartPos);
                Value* remainingSize = Builder.CreateZExt(remaining, sizeTy);
                
                Value* foundPtr = Builder.CreateCall(memchrFn, {searchPtr, filterCharVal, remainingSize});
                Value* isNull = Builder.CreateICmpEQ(foundPtr, ConstantPointerNull::get(cast<PointerType>(i8ptrTy)));
                Builder.CreateCondBr(isNull, ReturnFailBB, MemchrFoundBB);
                
                // === MEMCHR FOUND BLOCK ===
                // Calculate position of found char
                Builder.SetInsertPoint(MemchrFoundBB);
                Value* foundPtrInt = Builder.CreatePtrToInt(foundPtr, sizeTy);
                Value* arg0PtrInt = Builder.CreatePtrToInt(Arg0, sizeTy);
                Value* foundPosLong = Builder.CreateSub(foundPtrInt, arg0PtrInt);
                Value* foundPos = Builder.CreateTrunc(foundPosLong, Builder.getInt32Ty());
                
                // Set range: try positions from RangeStart to foundPos (inclusive)
                Value* rangeStart = Builder.CreateLoad(Builder.getInt32Ty(), RangeStartAlloca);
                Builder.CreateStore(foundPos, RangeEndAlloca);
                Builder.CreateStore(rangeStart, Index);  // Start trying from rangeStart
                Builder.CreateBr(RangeLoopCheckBB);
                
                // === RANGE LOOP CHECK ===
                // Check if we've tried all positions in the range
                Builder.SetInsertPoint(RangeLoopCheckBB);
                Value* curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
                Value* rangeEnd = Builder.CreateLoad(Builder.getInt32Ty(), RangeEndAlloca);
                Value* inRange = Builder.CreateICmpSLE(curIdx, rangeEnd);
                Builder.CreateCondBr(inRange, RangeLoopBodyBB, NextMemchrBB);
                
                // === RANGE LOOP BODY ===
                // Try to match at current position
                Builder.SetInsertPoint(RangeLoopBodyBB);
                BasicBlock *TrySuccess = BasicBlock::Create(Context, "try_success", MatchF);
                BasicBlock *TryFail = BasicBlock::Create(Context, "try_fail", MatchF);
                
                Value* tryIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
                Builder.CreateStore(tryIdx, Index);
                
                Body->SetFailBlock(TryFail);
                Body->SetSuccessBlock(TrySuccess);
                Body->CodeGen();
                
                // Success - return 1
                Builder.SetInsertPoint(TrySuccess);
                Builder.CreateBr(ReturnSuccessBB);
                
                // Fail - try next position in range
                Builder.SetInsertPoint(TryFail);
                Value* nextIdx = Builder.CreateAdd(tryIdx, ConstantInt::get(Builder.getInt32Ty(), 1));
                Builder.CreateStore(nextIdx, Index);
                Builder.CreateBr(RangeLoopCheckBB);
                
                // === NEXT MEMCHR ===
                // Move to find next occurrence of required char
                Builder.SetInsertPoint(NextMemchrBB);
                Value* nextRangeEnd = Builder.CreateLoad(Builder.getInt32Ty(), RangeEndAlloca);
                Value* nextMemchrPos = Builder.CreateAdd(nextRangeEnd, ConstantInt::get(Builder.getInt32Ty(), 1));
                Builder.CreateStore(nextMemchrPos, MemchrPosAlloca);
                Builder.CreateStore(nextMemchrPos, RangeStartAlloca);  // Next range starts after this one
                Builder.CreateBr(MemchrSearchBB);
                
            } else {
                // === BASIC SEARCH LOOP (no required chars to accelerate) ===
                BasicBlock *LoopCheckBB = BasicBlock::Create(Context, "search_loop_check", MatchF);
                BasicBlock *LoopBodyBB = BasicBlock::Create(Context, "search_loop_body", MatchF);
                BasicBlock *LoopIncBB = BasicBlock::Create(Context, "search_loop_inc", MatchF);

                Builder.CreateBr(LoopCheckBB);

                // Search loop condition: for(curIdx=0; curIdx<=strlen; ++curIdx)
                Builder.SetInsertPoint(LoopCheckBB);
                Value *curIdx_search = Builder.CreateLoad(Builder.getInt32Ty(), Index);
                Value *cond = Builder.CreateICmpSLE(curIdx_search, strlenVal);
                Builder.CreateCondBr(cond, LoopBodyBB, ReturnFailBB);

                // Each search attempt gets its own AST success/fail blocks
                Builder.SetInsertPoint(LoopBodyBB);
                BasicBlock *TrySuccess = BasicBlock::Create(Context, "try_success", MatchF);
                BasicBlock *TryFail = BasicBlock::Create(Context, "try_fail", MatchF);
                
                Builder.CreateStore(curIdx_search, Index);

                Body->SetFailBlock(TryFail);
                Body->SetSuccessBlock(TrySuccess);
                Body->CodeGen();

                // If body succeeds, return success immediately
                Builder.SetInsertPoint(TrySuccess);
                Builder.CreateBr(ReturnSuccessBB);

                // If body fails, go to increment and try next index
                Builder.SetInsertPoint(TryFail);
                Builder.CreateBr(LoopIncBB);

                // Search loop increment: curIdx++ and loop back
                Builder.SetInsertPoint(LoopIncBB);
                Value* nextIdx_search = Builder.CreateAdd(curIdx_search, ConstantInt::get(Context, APInt(32, 1)));
                Builder.CreateStore(nextIdx_search, Index);
                Builder.CreateBr(LoopCheckBB);
            }
        }
    }

    // Define the actual return blocks
    Builder.SetInsertPoint(ReturnSuccessBB);
    Builder.CreateRet(ConstantInt::get(Context, APInt(32, 1)));
    
    Builder.SetInsertPoint(ReturnFailBB);
    Builder.CreateRet(ConstantInt::get(Context, APInt(32, 0)));
    
    // Reset the global context/builder pointers after all IR generation is done
    ContextPtr = &GlobalContext;
    BuilderPtr = &GlobalBuilder;
    
    return nullptr;
}

// Match::CodeGen - 匹配单个字符
Value* Match::CodeGen() {
    // 获取当前索引
    Value* idx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
    // 获取字符指针
    Value* charPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, {idx});
    // 加载当前字符
    Value* ch = Builder.CreateLoad(Builder.getInt8Ty(), charPtr);
    // 比较字符
    Value* expected = ConstantInt::get(Context, APInt(8, (uint8_t)choice));
    Value* cmp = Builder.CreateICmpEQ(ch, expected);
    
    // 创建成功块 - 增加索引并跳转到下一个
    BasicBlock* matchSuccess = BasicBlock::Create(Context, "match_success", MatchF);
    Builder.CreateCondBr(cmp, matchSuccess, GetFailBlock());
    
    Builder.SetInsertPoint(matchSuccess);
    // 增加索引
    Value* nextIdx = Builder.CreateAdd(idx, ConstantInt::get(Context, APInt(32, 1)));
    Builder.CreateStore(nextIdx, Index);
    Builder.CreateBr(GetSuccessBlock());
    
    return nullptr;
}

// Concat::Append - 添加元素到连接序列
void Concat::Append(std::unique_ptr<Root> Body) {
    BodyVec.push_back(std::move(Body));
}

// Concat::CodeGen - 连接多个模式
Value* Concat::CodeGen() {
    if (BodyVec.empty()) {
        Builder.CreateBr(GetSuccessBlock());
        return nullptr;
    }
    
    // 创建各元素之间的连接块
    std::vector<BasicBlock*> blocks;
    for (size_t i = 0; i < BodyVec.size(); ++i) {
        blocks.push_back(BasicBlock::Create(Context, "concat_" + std::to_string(i), MatchF));
    }
    
    // 跳转到第一个块
    Builder.CreateBr(blocks[0]);
    
    // 生成每个元素的代码
    for (size_t i = 0; i < BodyVec.size(); ++i) {
        Builder.SetInsertPoint(blocks[i]);
        // 设置成功块：下一个元素或最终成功块
        if (i + 1 < BodyVec.size()) {
            BodyVec[i]->SetSuccessBlock(blocks[i + 1]);
        } else {
            BodyVec[i]->SetSuccessBlock(GetSuccessBlock());
        }
        // 失败块：整体失败
        BodyVec[i]->SetFailBlock(GetFailBlock());
        BodyVec[i]->CodeGen();
    }
    
    return nullptr;
}

// Alternative::Append - 添加元素到选择序列
void Alternative::Append(std::unique_ptr<Root> Body) {
    BodyVec.push_back(std::move(Body));
}

// Alternative::CodeGen - 选择操作 (|)
Value* Alternative::CodeGen() {
    if (BodyVec.empty()) {
        Builder.CreateBr(GetFailBlock());
        return nullptr;
    }
    
    // 只有一个选项时直接执行
    if (BodyVec.size() == 1) {
        BodyVec[0]->SetSuccessBlock(GetSuccessBlock());
        BodyVec[0]->SetFailBlock(GetFailBlock());
        BodyVec[0]->CodeGen();
        return nullptr;
    }
    
    // 创建每个选项的尝试块和失败后尝试下一个的块
    std::vector<BasicBlock*> tryBlocks;
    for (size_t i = 0; i < BodyVec.size(); ++i) {
        tryBlocks.push_back(BasicBlock::Create(Context, "alt_try_" + std::to_string(i), MatchF));
    }
    
    // 跳转到第一个选项
    Builder.CreateBr(tryBlocks[0]);
    
    // 生成每个选项的代码
    for (size_t i = 0; i < BodyVec.size(); ++i) {
        Builder.SetInsertPoint(tryBlocks[i]);
        
        // 保存当前索引以便回溯
        Value* savedIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
        
        // 创建回溯块
        BasicBlock* restoreBlock = nullptr;
        if (i + 1 < BodyVec.size()) {
            restoreBlock = BasicBlock::Create(Context, "alt_restore_" + std::to_string(i), MatchF);
        }
        
        // 设置成功块和失败块
        BodyVec[i]->SetSuccessBlock(GetSuccessBlock());
        if (i + 1 < BodyVec.size()) {
            BodyVec[i]->SetFailBlock(restoreBlock);
        } else {
            // 最后一个选项失败就是整体失败
            BodyVec[i]->SetFailBlock(GetFailBlock());
        }
        BodyVec[i]->CodeGen();
        
        // 如果不是最后一个选项，生成回溯块
        if (restoreBlock) {
            Builder.SetInsertPoint(restoreBlock);
            // 恢复索引
            Builder.CreateStore(savedIdx, Index);
            // 尝试下一个选项
            Builder.CreateBr(tryBlocks[i + 1]);
        }
    }
    
    return nullptr;
}

Value* Repeat::CodeGen() {
    auto intTy = Builder.getInt32Ty();
    // Star: minCount=0, maxCount=-1
    // Plus: minCount=1, maxCount=-1
    bool isStar = (minCount == 0 && maxCount == -1);
    bool isPlus = (minCount == 1 && maxCount == -1);
    Value* savedIdx = Builder.CreateAlloca(intTy);
    
    if (isStar || isPlus) {
        // For zero-width bodies (anchors, lookarounds), we need special handling
        // to prevent infinite loops. 
        bool bodyIsZeroWidth = Body->isZeroWidth();
        
        // Special case: zero-width Plus - just match once and succeed
        if (isPlus && bodyIsZeroWidth) {
            // Must match exactly once, then succeed
            Body->SetSuccessBlock(GetSuccessBlock());
            Body->SetFailBlock(GetFailBlock());
            Body->CodeGen();
            return nullptr;
        }
        
        // Special case: zero-width Star - try to match once, always succeed
        if (isStar && bodyIsZeroWidth) {
            // Create blocks for the attempt
            BasicBlock* tryBlock = BasicBlock::Create(Context, "repeat_zero_try", MatchF);
            BasicBlock* afterBlock = BasicBlock::Create(Context, "repeat_zero_after", MatchF);
            
            Builder.CreateBr(tryBlock);
            Builder.SetInsertPoint(tryBlock);
            
            // Try to match once
            Body->SetSuccessBlock(afterBlock);
            Body->SetFailBlock(afterBlock);  // Fail also goes to after (zero times is OK)
            Body->CodeGen();
            
            // After trying, go to success
            Builder.SetInsertPoint(afterBlock);
            Builder.CreateBr(GetSuccessBlock());
            return nullptr;
        }
        
        // === FAST PATH: Single character repeat (a+, a*, b+, etc.) ===
        // Use regjit_count_char to count consecutive matching chars in one call
        int singleChar = Body->getSingleChar();
        if (singleChar >= 0 && !nonGreedy) {
            // Fast path for greedy single-char quantifiers
            RJDBG(std::cerr << "Using fast path for single-char repeat: " << (char)singleChar << (isPlus ? "+" : "*") << "\n");
            
            Type* i8ptrTy = PointerType::get(Builder.getInt8Ty(), 0);
            Type* sizeTy = Builder.getInt64Ty();
            
            // Declare regjit_count_char: size_t regjit_count_char(const char* str, size_t len, char target)
            FunctionCallee countFn = ThisModule->getOrInsertFunction("regjit_count_char",
                FunctionType::get(sizeTy, {i8ptrTy, sizeTy, Builder.getInt8Ty()}, false));
            
            // Get current position and remaining length
            Value* curIdx = Builder.CreateLoad(intTy, Index);
            Value* strLen = Builder.CreateLoad(intTy, StrLenAlloca);
            Value* remaining = Builder.CreateSub(strLen, curIdx);
            Value* remainingSize = Builder.CreateZExt(remaining, sizeTy);
            
            // Get pointer to current position
            Value* curPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, {curIdx});
            
            // Call regjit_count_char(curPtr, remaining, targetChar)
            Value* targetChar = ConstantInt::get(Builder.getInt8Ty(), singleChar);
            Value* count = Builder.CreateCall(countFn, {curPtr, remainingSize, targetChar});
            Value* count32 = Builder.CreateTrunc(count, intTy);
            
            if (isPlus) {
                // Plus: must match at least one
                Value* isZero = Builder.CreateICmpEQ(count32, ConstantInt::get(intTy, 0));
                BasicBlock* successBlock = BasicBlock::Create(Context, "repeat_fast_success", MatchF);
                Builder.CreateCondBr(isZero, GetFailBlock(), successBlock);
                
                Builder.SetInsertPoint(successBlock);
                Value* newIdx = Builder.CreateAdd(curIdx, count32);
                Builder.CreateStore(newIdx, Index);
                Builder.CreateBr(GetSuccessBlock());
            } else {
                // Star: zero or more is always ok
                Value* newIdx = Builder.CreateAdd(curIdx, count32);
                Builder.CreateStore(newIdx, Index);
                Builder.CreateBr(GetSuccessBlock());
            }
            return nullptr;
        }
        
        // Regular (non-zero-width) body handling
        BasicBlock* checkBlock = BasicBlock::Create(Context, "repeat_check", MatchF);
        BasicBlock* bodyBlock = BasicBlock::Create(Context, "repeat_body", MatchF);
        BasicBlock* exitBlock = BasicBlock::Create(Context, "repeat_exit", MatchF);

        if (!isStar) { // Plus: must match at least once
            Value* curIdx = Builder.CreateLoad(intTy, Index);
            Builder.CreateStore(curIdx, savedIdx);
            BasicBlock* firstFailRestore = BasicBlock::Create(Context, "repeat_first_fail_restore", MatchF);
            Body->SetSuccessBlock(checkBlock);
            Body->SetFailBlock(firstFailRestore);
            Body->CodeGen();
            Builder.SetInsertPoint(firstFailRestore);
            Value* restore = Builder.CreateLoad(intTy, savedIdx);
            Builder.CreateStore(restore, Index);
            Builder.CreateBr(GetFailBlock());
        } else { // Star: can match zero times
            Builder.CreateBr(checkBlock);
        }

        Builder.SetInsertPoint(checkBlock);

        BasicBlock* failRestore = BasicBlock::Create(Context, "repeat_fail_restore", MatchF);
        if (nonGreedy) {
            // Non-greedy: first try to exit, then try to match
            Builder.CreateCondBr(Builder.getTrue(), GetSuccessBlock(), bodyBlock);
        } else {
            // Greedy: first try to match, then exit on failure
            Body->SetSuccessBlock(checkBlock); // Loop back on success
            Body->SetFailBlock(failRestore);   // Restore index then exit
            Builder.CreateBr(bodyBlock);
        }

        Builder.SetInsertPoint(bodyBlock);
        Value* curIdx = Builder.CreateLoad(intTy, Index);
        Builder.CreateStore(curIdx, savedIdx);
        Body->CodeGen();
        if (Builder.GetInsertBlock()->getTerminator() == nullptr) {
          if (nonGreedy) Builder.CreateBr(checkBlock);
          else Builder.CreateBr(failRestore);
        }

        Builder.SetInsertPoint(failRestore);
        Value* restore = Builder.CreateLoad(intTy, savedIdx);
        Builder.CreateStore(restore, Index);
        Builder.CreateBr(exitBlock);

        Builder.SetInsertPoint(exitBlock);
        Builder.CreateBr(GetSuccessBlock());
        return nullptr;
    }
    
    // === FAST PATH: Single character exact/range repeat (a{n}, a{n,m}) ===
    // For patterns like a{1000}, use regjit_count_char instead of looping
    int singleChar = Body->getSingleChar();
    bool isExactRepeat = (minCount == maxCount && minCount > 0);
    bool isRangeRepeat = (minCount >= 0 && maxCount > minCount);
    bool isMinOnlyRepeat = (minCount > 0 && maxCount == -1);  // a{n,}
    
    if (singleChar >= 0 && !nonGreedy && (isExactRepeat || isRangeRepeat || isMinOnlyRepeat)) {
        RJDBG(std::cerr << "Using fast path for char repeat: " << (char)singleChar << "{" << minCount << "," << maxCount << "}\n");
        
        Type* i8ptrTy = PointerType::get(Builder.getInt8Ty(), 0);
        Type* sizeTy = Builder.getInt64Ty();
        
        // Declare regjit_count_char
        FunctionCallee countFn = ThisModule->getOrInsertFunction("regjit_count_char",
            FunctionType::get(sizeTy, {i8ptrTy, sizeTy, Builder.getInt8Ty()}, false));
        
        // Get current position and remaining length
        Value* curIdx = Builder.CreateLoad(intTy, Index);
        Value* strLen = Builder.CreateLoad(intTy, StrLenAlloca);
        Value* remaining = Builder.CreateSub(strLen, curIdx);
        Value* remainingSize = Builder.CreateZExt(remaining, sizeTy);
        
        // Get pointer to current position
        Value* curPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, {curIdx});
        
        // Call regjit_count_char
        Value* targetChar = ConstantInt::get(Builder.getInt8Ty(), singleChar);
        Value* count = Builder.CreateCall(countFn, {curPtr, remainingSize, targetChar});
        Value* count32 = Builder.CreateTrunc(count, intTy);
        
        // Check minimum requirement
        Value* minVal = ConstantInt::get(intTy, minCount);
        Value* hasEnough = Builder.CreateICmpSGE(count32, minVal);
        
        BasicBlock* successBlock = BasicBlock::Create(Context, "repeat_range_success", MatchF);
        Builder.CreateCondBr(hasEnough, successBlock, GetFailBlock());
        
        Builder.SetInsertPoint(successBlock);
        
        // Calculate how many to consume (greedy: consume up to max, or all if unbounded)
        Value* consumed;
        if (maxCount == -1) {
            // a{n,} - consume all matched
            consumed = count32;
        } else {
            // a{n,m} or a{n} - consume min(count, max)
            Value* maxVal = ConstantInt::get(intTy, maxCount);
            Value* useMax = Builder.CreateICmpSGT(count32, maxVal);
            consumed = Builder.CreateSelect(useMax, maxVal, count32);
        }
        
        Value* newIdx = Builder.CreateAdd(curIdx, consumed);
        Builder.CreateStore(newIdx, Index);
        Builder.CreateBr(GetSuccessBlock());
        return nullptr;
    }
    
    // 允许范围（如 {2,5} 或 {3,} ）
    int minR = minCount < 0 ? 0 : minCount;
    int maxR = maxCount;
    // max = -1 视为无穷(贪婪型)
    Value* counter = Builder.CreateAlloca(intTy);
    Builder.CreateStore(ConstantInt::get(Context, APInt(32, 0)), counter);
    BasicBlock* checkMin = BasicBlock::Create(Context, "repeat_min_chk", MatchF);
    BasicBlock* incMin = BasicBlock::Create(Context, "repeat_min", MatchF);
    BasicBlock* checkMax = BasicBlock::Create(Context, "repeat_max_chk", MatchF);
    BasicBlock* incMax = BasicBlock::Create(Context, "repeat_max", MatchF);
    BasicBlock* exit = BasicBlock::Create(Context, "repeat_exit_rng", MatchF);
    // 首先循环min次
    Builder.CreateBr(checkMin);
    // check min loop
    Builder.SetInsertPoint(checkMin);
    Value* val = Builder.CreateLoad(intTy, counter);
    Value* mincheck = Builder.CreateICmpSLT(val, ConstantInt::get(Context, APInt(32, minR)));
    Builder.CreateCondBr(mincheck, incMin, checkMax);
    // min loop体: incMin is the block where we attempt one repetition.
    // Create a dedicated post-success increment block so we always update
    // the counter when the Body succeeds (Body may emit a terminator that
    // jumps elsewhere, so we cannot rely on emitting increments after
    // Body->CodeGen in the same block).
    BasicBlock* incMinSuccess = BasicBlock::Create(Context, "repeat_min_inc_success", MatchF);
    Builder.SetInsertPoint(incMin);
    Body->SetSuccessBlock(incMinSuccess);
    Body->SetFailBlock(GetFailBlock());
    Body->CodeGen();
    // If Body fell through without emitting a terminator, ensure we branch
    // to the success-increment block so behavior is consistent.
    if (Builder.GetInsertBlock()->getTerminator() == nullptr) {
        Builder.CreateBr(incMinSuccess);
    }
    // Emit the increment and loop-back in the dedicated inc-success block
    Builder.SetInsertPoint(incMinSuccess);
    Value* stepmin = Builder.CreateAdd(val, ConstantInt::get(Context, APInt(32,1)));
    Builder.CreateStore(stepmin, counter);
    // Index advancement is done by the consuming node; do not modify Index here.
    Builder.CreateBr(checkMin);
    // min循环完后可进入max部分
    Builder.SetInsertPoint(checkMax);
    Value* val2 = Builder.CreateLoad(intTy, counter);
    Value* finished = maxR == -1 ? Builder.getFalse() : Builder.CreateICmpSGE(val2, ConstantInt::get(Context, APInt(32, maxR)));
    Builder.CreateCondBr(finished, exit, incMax);
    // max阶段：贪婪与非贪婪切分分支
    Builder.SetInsertPoint(incMax);
    // Create an explicit attempt block so we don't emit instructions after
    // a terminating branch in the current block which would produce
    // invalid IR and crash optimizer passes.
    BasicBlock* attemptBlock = BasicBlock::Create(Context, "repeat_attempt", MatchF);
    BasicBlock* attemptSuccessInc = BasicBlock::Create(Context, "repeat_attempt_inc", MatchF);
    // Branch to attemptBlock (or to success immediately for non-greedy path)
    if (nonGreedy) {
        // Non-greedy: first try to succeed without consuming; if that fails,
        // attempt another repetition.
        Builder.CreateBr(GetSuccessBlock());
        // Now emit the attempt block which will try another repetition
        Builder.SetInsertPoint(attemptBlock);
        Body->SetSuccessBlock(attemptSuccessInc);
        Body->SetFailBlock(exit);
        Body->CodeGen();
        if (Builder.GetInsertBlock()->getTerminator() == nullptr) {
            Builder.CreateBr(attemptSuccessInc);
        }
        // attemptSuccessInc: increment and go back to checkMax
        Builder.SetInsertPoint(attemptSuccessInc);
        Value* afterVal = Builder.CreateLoad(intTy, counter);
        Value* stepAfter = Builder.CreateAdd(afterVal, ConstantInt::get(Context, APInt(32,1)));
        Builder.CreateStore(stepAfter, counter);
        // Index advancement is done by the consuming node; do not modify Index here.
        Builder.CreateBr(checkMax);
    } else {
        // Greedy: attempt to consume first, then on fail try overall success
        Builder.CreateBr(attemptBlock);
        Builder.SetInsertPoint(attemptBlock);
        Body->SetSuccessBlock(attemptSuccessInc);
        Body->SetFailBlock(exit);
        Body->CodeGen();
        if (Builder.GetInsertBlock()->getTerminator() == nullptr) {
            Builder.CreateBr(GetSuccessBlock());
        }
        // After attempting to consume, increment and loop back
        Builder.SetInsertPoint(attemptSuccessInc);
        Value* afterVal2 = Builder.CreateLoad(intTy, counter);
        Value* stepAfter2 = Builder.CreateAdd(afterVal2, ConstantInt::get(Context, APInt(32,1)));
        Builder.CreateStore(stepAfter2, counter);
        // Index advancement is done by the consuming node; do not modify Index here.
        Builder.CreateBr(checkMax);
    }
    // 量词退出
    Builder.SetInsertPoint(exit);
    Builder.CreateBr(GetSuccessBlock());
    return nullptr;
}

// New lexical analyzer
class RegexLexer {
    const char* m_ptr;
    char m_cur_char;
public:
    explicit RegexLexer(const std::string& input) 
        : m_ptr(input.c_str()), m_cur_char(*m_ptr) {}

    char next() {
        m_cur_char = (*(++m_ptr)) ? *m_ptr : 0;
        return m_cur_char;
    }

    char current() const { return m_cur_char; }
    
    bool is_end() const { return m_cur_char == 0; }

    enum TokenType {
        CHAR,
        STAR,   // *
        PLUS,   // +
        QMARK,  // ?
        PIPE,   // |
        LPAREN, // (
        RPAREN, // )
        LBRACKET, // [
        RBRACKET, // ]
        LBRACE,   // {
        RBRACE,   // }
        COMMA,    // ,
        DASH,    // -
        CARET,   // ^ (inside character class)
        CARET_ANCHOR, // ^ (line start anchor)
        DOLLAR,  // $ (line end anchor)
        DOT,     // .
        BACKSLASH, // \\ (escape sequence start)
        WORD_BOUNDARY,    // \\b
        NON_WORD_BOUNDARY, // \\B
        // Character class escapes
        DIGIT_CLASS,      // \\d - [0-9]
        NON_DIGIT_CLASS,  // \\D - [^0-9]
        WORD_CLASS,       // \\w - [a-zA-Z0-9_]
        NON_WORD_CLASS,   // \\W - [^a-zA-Z0-9_]
        SPACE_CLASS,      // \\s - [ \\t\\n\\r\\f\\v]
        NON_SPACE_CLASS,  // \\S - [^ \\t\\n\\r\\f\\v]
        EOS      // End of string
    };

    struct Token {
        TokenType type;
        char value;
    };

    Token get_next_token() {
        while (!is_end()) {
            switch (current()) {
                case '*': next(); return {STAR, '*'};
                case '+': next(); return {PLUS, '+'};
                case '?': next(); return {QMARK, '?'};
                case '|': next(); return {PIPE, '|'};
                case '(': next(); return {LPAREN, '('};
                case ')': next(); return {RPAREN, ')'};
                case '[': next(); return {LBRACKET, '['};
                case ']': next(); return {RBRACKET, ']'};
                case '{': next(); return {LBRACE, '{'};
                case '}': next(); return {RBRACE, '}'};
                case ',': next(); return {COMMA, ','};
                case '-': next(); return {DASH, '-'};
                case '^': next(); return {CARET, '^'}; // Will be handled by parser based on context
                case '$': next(); return {DOLLAR, '$'};
                case '.': next(); return {DOT, '.'};
                case '\\': { // Handle escape characters
                    next();
                    char c = current();
                    if (is_end()) break;
                    next();
                    // Return special tokens for anchors
                    if (c == 'b') return {WORD_BOUNDARY, 'b'};
                    if (c == 'B') return {NON_WORD_BOUNDARY, 'B'};
                    // Character class escapes
                    if (c == 'd') return {DIGIT_CLASS, 'd'};
                    if (c == 'D') return {NON_DIGIT_CLASS, 'D'};
                    if (c == 'w') return {WORD_CLASS, 'w'};
                    if (c == 'W') return {NON_WORD_CLASS, 'W'};
                    if (c == 's') return {SPACE_CLASS, 's'};
                    if (c == 'S') return {NON_SPACE_CLASS, 'S'};
                    // Literal escape characters
                    if (c == 't') return {CHAR, '\t'};
                    if (c == 'n') return {CHAR, '\n'};
                    if (c == 'r') return {CHAR, '\r'};
                    if (c == 'f') return {CHAR, '\f'};
                    if (c == 'v') return {CHAR, '\v'};
                    if (c == '0') return {CHAR, '\0'};
                    // Any other escaped character is literal
                    return {CHAR, c};
                }
                default:
                    if (isspace(current())) {
                        next();
                        continue;
                    }
                    char c = current();
                    next();
                    return {CHAR, c};
            }
        }
        return {EOS, 0};
    }
};

// New syntax analyzer
class RegexParser {
    RegexLexer& m_lexer;
    RegexLexer::Token m_cur_token;

    std::unique_ptr<Root> parse_character_class() {
        // Skip opening bracket
        m_cur_token = m_lexer.get_next_token();
        
        bool negated = false;
        if (m_cur_token.type == RegexLexer::CARET) {
            negated = true;
            m_cur_token = m_lexer.get_next_token();
        }

        if (m_cur_token.type == RegexLexer::RBRACKET) {
            throw std::runtime_error("unterminated character set");
        }
        
        auto charClass = std::make_unique<CharClass>(negated, false);
        
        // Parse characters and ranges
        while (m_cur_token.type != RegexLexer::RBRACKET) {
            if (m_cur_token.type == RegexLexer::EOS) {
                throw std::runtime_error("Unclosed character class");
            }
            
            if (m_cur_token.type == RegexLexer::CHAR) {
                char startChar = m_cur_token.value;
                m_cur_token = m_lexer.get_next_token();
                
                // Check for range
                if (m_cur_token.type == RegexLexer::DASH) {
                    m_cur_token = m_lexer.get_next_token();
                    if (m_cur_token.type != RegexLexer::CHAR) {
                        throw std::runtime_error("Invalid range in character class");
                    }
                    char endChar = m_cur_token.value;
                    if (endChar < startChar) {
                        throw std::runtime_error("bad character range");
                    }
                    m_cur_token = m_lexer.get_next_token();
                    
                    // Add range
                    charClass->addRange(startChar, endChar, true);
                } else {
                    // Add single character
                    charClass->addChar(startChar, true);
                }
            } else {
                throw std::runtime_error("Unexpected token in character class");
            }
        }
        
        // Skip closing bracket
        m_cur_token = m_lexer.get_next_token();
        return charClass;
    }

    std::unique_ptr<Root> parse_element() {
    if (m_cur_token.type == RegexLexer::STAR ||
        m_cur_token.type == RegexLexer::PLUS ||
        m_cur_token.type == RegexLexer::QMARK ||
        m_cur_token.type == RegexLexer::LBRACE) {
            throw std::runtime_error("nothing to repeat");
        }
    if (m_cur_token.type == RegexLexer::RPAREN) {
            throw std::runtime_error("unbalanced parenthesis");
        }
    if (m_cur_token.type == RegexLexer::LPAREN) {
            // Support normal groups and non-capturing groups like (?:...)
            m_cur_token = m_lexer.get_next_token();
            if (m_cur_token.type == RegexLexer::QMARK) {
                // Expect ':' for non-capturing group
                m_cur_token = m_lexer.get_next_token();
                if (!(m_cur_token.type == RegexLexer::CHAR && m_cur_token.value == ':')) {
                    throw std::runtime_error("Unsupported group modifier");
                }
                // Move to next token after ':' and parse group body
                m_cur_token = m_lexer.get_next_token();
                auto expr = parse_expr();
                if (m_cur_token.type != RegexLexer::RPAREN) {
                    throw std::runtime_error("Mismatched parentheses");
                }
                m_cur_token = m_lexer.get_next_token();
                return expr;
            } else {
                auto expr = parse_expr();
                if (m_cur_token.type != RegexLexer::RPAREN) {
                    throw std::runtime_error("Mismatched parentheses");
                }
                m_cur_token = m_lexer.get_next_token();
                return expr;
            }
        } else if (m_cur_token.type == RegexLexer::DOT) {
            m_cur_token = m_lexer.get_next_token();
            return std::make_unique<CharClass>(false, true); // dot class
        } else if (m_cur_token.type == RegexLexer::LBRACKET) {
            return parse_character_class();
        } else if (m_cur_token.type == RegexLexer::CARET) {
            m_cur_token = m_lexer.get_next_token();
            return std::make_unique<Anchor>(Anchor::Start); // ^ anchor
        } else if (m_cur_token.type == RegexLexer::DOLLAR) {
            m_cur_token = m_lexer.get_next_token();
            return std::make_unique<Anchor>(Anchor::End); // $ anchor
        } else if (m_cur_token.type == RegexLexer::WORD_BOUNDARY) {
            m_cur_token = m_lexer.get_next_token();
            return std::make_unique<Anchor>(Anchor::WordBoundary); // \b
        } else if (m_cur_token.type == RegexLexer::NON_WORD_BOUNDARY) {
            m_cur_token = m_lexer.get_next_token();
            return std::make_unique<Anchor>(Anchor::NonWordBoundary); // \B
        } else if (m_cur_token.type == RegexLexer::DIGIT_CLASS) {
            // \d - [0-9]
            m_cur_token = m_lexer.get_next_token();
            auto cc = std::make_unique<CharClass>(false, false);
            cc->addRange('0', '9');
            return cc;
        } else if (m_cur_token.type == RegexLexer::NON_DIGIT_CLASS) {
            // \D - [^0-9]
            m_cur_token = m_lexer.get_next_token();
            auto cc = std::make_unique<CharClass>(true, false); // negated
            cc->addRange('0', '9');
            return cc;
        } else if (m_cur_token.type == RegexLexer::WORD_CLASS) {
            // \w - [a-zA-Z0-9_]
            m_cur_token = m_lexer.get_next_token();
            auto cc = std::make_unique<CharClass>(false, false);
            cc->addRange('a', 'z');
            cc->addRange('A', 'Z');
            cc->addRange('0', '9');
            cc->addChar('_');
            return cc;
        } else if (m_cur_token.type == RegexLexer::NON_WORD_CLASS) {
            // \W - [^a-zA-Z0-9_]
            m_cur_token = m_lexer.get_next_token();
            auto cc = std::make_unique<CharClass>(true, false); // negated
            cc->addRange('a', 'z');
            cc->addRange('A', 'Z');
            cc->addRange('0', '9');
            cc->addChar('_');
            return cc;
        } else if (m_cur_token.type == RegexLexer::SPACE_CLASS) {
            // \s - [ \t\n\r\f\v]
            m_cur_token = m_lexer.get_next_token();
            auto cc = std::make_unique<CharClass>(false, false);
            cc->addChar(' ');
            cc->addChar('\t');
            cc->addChar('\n');
            cc->addChar('\r');
            cc->addChar('\f');
            cc->addChar('\v');
            return cc;
        } else if (m_cur_token.type == RegexLexer::NON_SPACE_CLASS) {
            // \S - [^ \t\n\r\f\v]
            m_cur_token = m_lexer.get_next_token();
            auto cc = std::make_unique<CharClass>(true, false); // negated
            cc->addChar(' ');
            cc->addChar('\t');
            cc->addChar('\n');
            cc->addChar('\r');
            cc->addChar('\f');
            cc->addChar('\v');
            return cc;
        } else if (m_cur_token.type == RegexLexer::CHAR) {
            char c = m_cur_token.value;
            m_cur_token = m_lexer.get_next_token();
            return std::make_unique<Match>(c);
        }
        throw std::runtime_error("Unexpected token");
    }

    std::unique_ptr<Root> parse_postfix() {
        auto node = parse_element();
        while (true) {
            switch (m_cur_token.type) {
                case RegexLexer::STAR: {
                    m_cur_token = m_lexer.get_next_token();
                    if (dynamic_cast<Repeat*>(node.get()) != nullptr) {
                        throw std::runtime_error("multiple repeat");
                    }
                    if (node->isZeroWidth()) {
                        throw std::runtime_error("nothing to repeat");
                    }
                    // 看后缀?判断贪婪/非贪婪
                    bool nongreedy = false;
                    if (m_cur_token.type == RegexLexer::QMARK) {
                        nongreedy = true;
                        m_cur_token = m_lexer.get_next_token();
                    }
                    node = Repeat::makeStar(std::move(node), nongreedy);
                    break;
                }
                case RegexLexer::PLUS: {
                    m_cur_token = m_lexer.get_next_token();
                    if (dynamic_cast<Repeat*>(node.get()) != nullptr) {
                        throw std::runtime_error("multiple repeat");
                    }
                    if (node->isZeroWidth()) {
                        throw std::runtime_error("nothing to repeat");
                    }
                    bool nongreedy = false;
                    if (m_cur_token.type == RegexLexer::QMARK) {
                        nongreedy = true;
                        m_cur_token = m_lexer.get_next_token();
                    }
                    node = Repeat::makePlus(std::move(node), nongreedy);
                    break;
                }
                case RegexLexer::QMARK: {
                    m_cur_token = m_lexer.get_next_token();
                    if (dynamic_cast<Repeat*>(node.get()) != nullptr) {
                        throw std::runtime_error("multiple repeat");
                    }
                    if (node->isZeroWidth()) {
                        throw std::runtime_error("nothing to repeat");
                    }
                    // 实现?量词: 转换为{0,1} 
                    bool nongreedy = false;
                    if (m_cur_token.type == RegexLexer::QMARK) {
                        nongreedy = true;
                        m_cur_token = m_lexer.get_next_token();
                    }
                    node = Repeat::makeRange(std::move(node), 0, 1, nongreedy);
                    break;
                }
                case RegexLexer::LBRACE: {
                    // 解析{n}, {n,}, {n,m}，处理非贪婪以及异常格式
                    int min = 0, max = -1;
                    bool nongreedy = false;
                    m_cur_token = m_lexer.get_next_token();
                    if (dynamic_cast<Repeat*>(node.get()) != nullptr) {
                        throw std::runtime_error("multiple repeat");
                    }
                    if (node->isZeroWidth()) {
                        throw std::runtime_error("nothing to repeat");
                    }
                    // 首先读min
                    if (m_cur_token.type != RegexLexer::CHAR || !isdigit(m_cur_token.value))
                        throw std::runtime_error("Malformed quantifier: expected digit after '{'");
                    min = m_cur_token.value - '0';
                    m_cur_token = m_lexer.get_next_token();
                    while (m_cur_token.type == RegexLexer::CHAR && isdigit(m_cur_token.value)) {
                        min = min * 10 + (m_cur_token.value - '0');
                        m_cur_token = m_lexer.get_next_token();
                    }
                    if (m_cur_token.type == RegexLexer::COMMA) {
                        // {n,}
                        m_cur_token = m_lexer.get_next_token();
                        if (m_cur_token.type == RegexLexer::CHAR && isdigit(m_cur_token.value)) {
                            // {n,m}
                            max = m_cur_token.value - '0';
                            m_cur_token = m_lexer.get_next_token();
                            while (m_cur_token.type == RegexLexer::CHAR && isdigit(m_cur_token.value)) {
                                max = max * 10 + (m_cur_token.value - '0');
                                m_cur_token = m_lexer.get_next_token();
                            }
                        }
                        // else {n,} 型, max已是-1
                    } else {
                        // {n} 精确
                        max = min;
                    }
                    if (m_cur_token.type != RegexLexer::RBRACE)
                        throw std::runtime_error("Malformed quantifier: missing '}'");
                    m_cur_token = m_lexer.get_next_token();
                    // 溯源贪婪/非贪婪?
                    if (m_cur_token.type == RegexLexer::QMARK) {
                        nongreedy = true;
                        m_cur_token = m_lexer.get_next_token();
                    }
                    if (min < 0 || (max >= 0 && max < min))
                        throw std::runtime_error("Malformed quantifier: nonsensical range");
                    node = Repeat::makeRange(std::move(node), min, max, nongreedy);
                    break;
                }
                default:
                    return node;
            }
        }
    }

    std::unique_ptr<Root> parse_concat() {
        auto left = parse_postfix();
    // Continue concatenation while next token can start an element. This
    // includes CHAR, group '(', dot '.', character class '[', anchor
    // tokens (^, $, \b, \B), and escape class tokens (\d, \D, \w, \W, \s, \S).
    // Previous implementation only checked for CHAR or LPAREN which caused
    // trailing anchors (like '$') to be left unconsumed and dropped from AST.
    while (m_cur_token.type == RegexLexer::CHAR ||
           m_cur_token.type == RegexLexer::LPAREN ||
           m_cur_token.type == RegexLexer::DOT ||
           m_cur_token.type == RegexLexer::LBRACKET ||
           m_cur_token.type == RegexLexer::CARET ||
           m_cur_token.type == RegexLexer::DOLLAR ||
           m_cur_token.type == RegexLexer::WORD_BOUNDARY ||
           m_cur_token.type == RegexLexer::NON_WORD_BOUNDARY ||
           // Escape sequence character classes
           m_cur_token.type == RegexLexer::DIGIT_CLASS ||
           m_cur_token.type == RegexLexer::NON_DIGIT_CLASS ||
           m_cur_token.type == RegexLexer::WORD_CLASS ||
           m_cur_token.type == RegexLexer::NON_WORD_CLASS ||
           m_cur_token.type == RegexLexer::SPACE_CLASS ||
           m_cur_token.type == RegexLexer::NON_SPACE_CLASS) {
            auto right = parse_postfix();
            auto concat = std::make_unique<Concat>();
            concat->Append(std::move(left));
            concat->Append(std::move(right));
            left = std::move(concat);
        }
        return left;
    }

    std::unique_ptr<Root> parse_expr() {
        auto left = parse_concat();
        while (m_cur_token.type == RegexLexer::PIPE) {
            m_cur_token = m_lexer.get_next_token();
            auto right = parse_concat();
            auto alt = std::make_unique<Alternative>();
            alt->Append(std::move(left));
            alt->Append(std::move(right));
            left = std::move(alt);
        }
        return left;
    }

public:
    explicit RegexParser(RegexLexer& lexer) 
        : m_lexer(lexer), m_cur_token(lexer.get_next_token()) {}

    std::unique_ptr<Root> parse() {
        return parse_expr();
    }
};

// CharClass implementation
Value* CharClass::CodeGen() {
    // Load current index
    Value* curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
    
    // CRITICAL: Boundary check - must have at least one character remaining
    // Without this, negated classes like \D would match '\0' at string end
    Value* strLen = Builder.CreateLoad(Builder.getInt32Ty(), StrLenAlloca);
    Value* inBounds = Builder.CreateICmpSLT(curIdx, strLen);
    
    BasicBlock* checkCharBlock = BasicBlock::Create(Context, "charclass_check", MatchF);
    BasicBlock* matchBlock = BasicBlock::Create(Context, "charclass_match", MatchF);
    BasicBlock* nomatchBlock = BasicBlock::Create(Context, "charclass_nomatch", MatchF);
    
    // If out of bounds, go directly to fail block
    Builder.CreateCondBr(inBounds, checkCharBlock, nomatchBlock);
    
    // Now we're in bounds, load and check the character
    Builder.SetInsertPoint(checkCharBlock);
    Value* charPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, curIdx);
    Value* currentChar = Builder.CreateLoad(Builder.getInt8Ty(), charPtr);
    currentChar = Builder.CreateIntCast(currentChar, Builder.getInt32Ty(), false);
    
    Value* finalMatch = nullptr;
    
    // For dot (.) class, match any character except newline
    if (dotClass) {
        Value* isNewline = Builder.CreateICmpEQ(currentChar, 
            ConstantInt::get(Context, APInt(32, '\n')));
        Value* isCarriageReturn = Builder.CreateICmpEQ(currentChar, 
            ConstantInt::get(Context, APInt(32, '\r')));
        Value* isLineEnd = Builder.CreateOr(isNewline, isCarriageReturn);
        finalMatch = Builder.CreateNot(isLineEnd);
    } else {
        // For regular character classes, check each range
        for (const auto& range : ranges) {
            Value* geStart = Builder.CreateICmpUGE(currentChar, 
                ConstantInt::get(Context, APInt(32, range.start)));
            Value* leEnd = Builder.CreateICmpULE(currentChar, 
                ConstantInt::get(Context, APInt(32, range.end)));
            Value* rangeMatch = Builder.CreateAnd(geStart, leEnd);
            
            if (!range.included) {
                rangeMatch = Builder.CreateNot(rangeMatch);
            }
            
            if (finalMatch == nullptr) {
                finalMatch = rangeMatch;
            } else {
                finalMatch = Builder.CreateOr(finalMatch, rangeMatch);
            }
        }
        
        // Apply negation if needed
        if (negated) {
            finalMatch = Builder.CreateNot(finalMatch);
        }
    }
    
    Builder.CreateCondBr(finalMatch, matchBlock, nomatchBlock);

    Builder.SetInsertPoint(matchBlock);
    // On match, increment Index (consuming node) then go to success
    if (!dotClass) {
        // For normal char classes we consume one char
        Value* curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
        Value* nextIdx = Builder.CreateAdd(curIdx, ConstantInt::get(Context, APInt(32, 1)));
        Builder.CreateStore(nextIdx, Index);
    } else {
        // dotClass also consumes one character
        Value* curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
        Value* nextIdx = Builder.CreateAdd(curIdx, ConstantInt::get(Context, APInt(32, 1)));
        Builder.CreateStore(nextIdx, Index);
    }
    Builder.CreateBr(GetSuccessBlock());
    
    Builder.SetInsertPoint(nomatchBlock);
    Builder.CreateBr(GetFailBlock());
    
    return nullptr;
}

// Anchor implementation
Value* Anchor::CodeGen() {
    Value* curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
    Value* match = nullptr;
    
    switch (anchorType) {
        case Start: {
            // ^ matches at the beginning of the string (index == 0)
            match = Builder.CreateICmpEQ(curIdx, 
                ConstantInt::get(Context, APInt(32, 0)));
            break;
        }
        case End: {
            // $ matches at the end of the string (index == strlen)
            // Use the precomputed inline strlen value stored in StrLenAlloca
            Value* strLen = Builder.CreateLoad(Builder.getInt32Ty(), StrLenAlloca);
            match = Builder.CreateICmpEQ(curIdx, strLen);
            break;
        }
        case WordBoundary: {
            // \b matches at word boundaries (transition between \w and \W)
            // Check if current position is at string boundaries
            // Use precomputed inline strlen value
            Value* strLen = Builder.CreateLoad(Builder.getInt32Ty(), StrLenAlloca);

            Value* atStart = Builder.CreateICmpEQ(curIdx, 
                ConstantInt::get(Context, APInt(32, 0)));
            Value* atEnd = Builder.CreateICmpEQ(curIdx, strLen);
            Value* atBoundary = Builder.CreateOr(atStart, atEnd);
            
            // If not at boundary, check character transition
            BasicBlock* checkTransition = BasicBlock::Create(Context, "check_transition", MatchF);
            BasicBlock* endCheck = BasicBlock::Create(Context, "end_check", MatchF);
            
            BasicBlock* currentBlock = Builder.GetInsertBlock();
            Builder.CreateCondBr(atBoundary, endCheck, checkTransition);
            
            // Check character transition
            Builder.SetInsertPoint(checkTransition);
            
            // Get current character
            Value* curCharPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, curIdx);
            Value* curChar = Builder.CreateLoad(Builder.getInt8Ty(), curCharPtr);
            curChar = Builder.CreateIntCast(curChar, Builder.getInt32Ty(), false);
            
            // Get previous character
            Value* prevIdx = Builder.CreateSub(curIdx, ConstantInt::get(Context, APInt(32, 1)));
            Value* prevCharPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, prevIdx);
            Value* prevChar = Builder.CreateLoad(Builder.getInt8Ty(), prevCharPtr);
            prevChar = Builder.CreateIntCast(prevChar, Builder.getInt32Ty(), false);
            
            // Check if characters are word characters (\w = [a-zA-Z0-9_])
            auto isWordChar = [&](Value* ch) -> Value* {
                Value* isLower = Builder.CreateAnd(
                    Builder.CreateICmpUGE(ch, ConstantInt::get(Context, APInt(32, 'a'))),
                    Builder.CreateICmpULE(ch, ConstantInt::get(Context, APInt(32, 'z')))
                );
                Value* isUpper = Builder.CreateAnd(
                    Builder.CreateICmpUGE(ch, ConstantInt::get(Context, APInt(32, 'A'))),
                    Builder.CreateICmpULE(ch, ConstantInt::get(Context, APInt(32, 'Z')))
                );
                Value* isDigit = Builder.CreateAnd(
                    Builder.CreateICmpUGE(ch, ConstantInt::get(Context, APInt(32, '0'))),
                    Builder.CreateICmpULE(ch, ConstantInt::get(Context, APInt(32, '9')))
                );
                Value* isUnderscore = Builder.CreateICmpEQ(ch, 
                    ConstantInt::get(Context, APInt(32, '_')));
                
                return Builder.CreateOr(Builder.CreateOr(Builder.CreateOr(isLower, isUpper), isDigit), isUnderscore);
            };
            
            Value* curIsWord = isWordChar(curChar);
            Value* prevIsWord = isWordChar(prevChar);
            
            // Word boundary: one is word char, the other is not
            Value* isBoundary = Builder.CreateXor(curIsWord, prevIsWord);
            
            Builder.CreateBr(endCheck);
            
// Combine results
            Builder.SetInsertPoint(endCheck);
            PHINode* result = Builder.CreatePHI(Builder.getInt1Ty(), 2);
            result->addIncoming(ConstantInt::get(Context, APInt(1, 1)), currentBlock);
            result->addIncoming(isBoundary, checkTransition);
            
            match = result;
            break;
        }
        case NonWordBoundary: {
            // \B matches at non-word boundaries
            // This is the negation of \b
            // Use precomputed inline strlen value
            Value* strLen = Builder.CreateLoad(Builder.getInt32Ty(), StrLenAlloca);

            Value* atStart = Builder.CreateICmpEQ(curIdx, 
                ConstantInt::get(Context, APInt(32, 0)));
            Value* atEnd = Builder.CreateICmpEQ(curIdx, strLen);
            Value* atBoundary = Builder.CreateOr(atStart, atEnd);
            
            // Special case: at string start/end with empty string (strLen == 0),
            // the position is non-word-to-non-word (both imaginary), so \B succeeds
            Value* isEmpty = Builder.CreateICmpEQ(strLen, ConstantInt::get(Context, APInt(32, 0)));
            
            // If at real boundary (non-empty string), fail immediately
            // If at empty string boundary, succeed
            // Otherwise check character transition
            BasicBlock* boundaryBlock = Builder.GetInsertBlock();  // Capture the block before branching
            BasicBlock* checkTransition = BasicBlock::Create(Context, "check_nonword_transition", MatchF);
            BasicBlock* endCheck = BasicBlock::Create(Context, "end_nonword_check", MatchF);
            
            // Branch: if (atBoundary && !isEmpty) goto endCheck with false, else goto checkTransition
            Value* realBoundary = Builder.CreateAnd(atBoundary, Builder.CreateNot(isEmpty));
            Builder.CreateCondBr(realBoundary, endCheck, checkTransition);
            
            // Check character transition (same as \b but negated)
            Builder.SetInsertPoint(checkTransition);
            
            // Get current character
            Value* curCharPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, curIdx);
            Value* curChar = Builder.CreateLoad(Builder.getInt8Ty(), curCharPtr);
            curChar = Builder.CreateIntCast(curChar, Builder.getInt32Ty(), false);
            
            // Get previous character
            Value* prevIdx = Builder.CreateSub(curIdx, ConstantInt::get(Context, APInt(32, 1)));
            Value* prevCharPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, prevIdx);
            Value* prevChar = Builder.CreateLoad(Builder.getInt8Ty(), prevCharPtr);
            prevChar = Builder.CreateIntCast(prevChar, Builder.getInt32Ty(), false);
            
            // Check if characters are word characters
            auto isWordChar = [&](Value* ch) -> Value* {
                Value* isLower = Builder.CreateAnd(
                    Builder.CreateICmpUGE(ch, ConstantInt::get(Context, APInt(32, 'a'))),
                    Builder.CreateICmpULE(ch, ConstantInt::get(Context, APInt(32, 'z')))
                );
                Value* isUpper = Builder.CreateAnd(
                    Builder.CreateICmpUGE(ch, ConstantInt::get(Context, APInt(32, 'A'))),
                    Builder.CreateICmpULE(ch, ConstantInt::get(Context, APInt(32, 'Z')))
                );
                Value* isDigit = Builder.CreateAnd(
                    Builder.CreateICmpUGE(ch, ConstantInt::get(Context, APInt(32, '0'))),
                    Builder.CreateICmpULE(ch, ConstantInt::get(Context, APInt(32, '9')))
                );
                Value* isUnderscore = Builder.CreateICmpEQ(ch, 
                    ConstantInt::get(Context, APInt(32, '_')));
                
                return Builder.CreateOr(Builder.CreateOr(Builder.CreateOr(isLower, isUpper), isDigit), isUnderscore);
            };
            
            Value* curIsWord = isWordChar(curChar);
            Value* prevIsWord = isWordChar(prevChar);
            
            // Non-word boundary: both are word chars or both are non-word chars
            Value* bothWord = Builder.CreateAnd(curIsWord, prevIsWord);
            Value* bothNonWord = Builder.CreateAnd(Builder.CreateNot(curIsWord), Builder.CreateNot(prevIsWord));
            Value* isNonBoundary = Builder.CreateOr(bothWord, bothNonWord);
            
              Builder.CreateBr(endCheck);
              
              // Combine results
              Builder.SetInsertPoint(endCheck);
              PHINode* result = Builder.CreatePHI(Builder.getInt1Ty(), 2);
              // Real boundary (non-empty string): fail
              result->addIncoming(ConstantInt::get(Context, APInt(1, 0)), boundaryBlock);
              // Character transition check result (or empty string case which goes through checkTransition)
              result->addIncoming(isNonBoundary, checkTransition);
              
              match = result;
            break;
        }
    }
    
    // Use the same pattern as Match::CodeGen - connect to fail/success blocks
    Builder.CreateCondBr(match, GetSuccessBlock(), GetFailBlock());
    
    return nullptr;
}

// Not operator implementation (negation of a subpattern)
// Note: This is a placeholder - the Not class is declared but not currently
// used by the parser. If you need negative lookahead or similar features,
// implement the actual logic here.
Value* Not::CodeGen() {
    // For now, just generate the body and invert the result
    // This is a stub implementation to satisfy the linker
    if (Body) {
        Body->CodeGen();
    }
    return nullptr;
}

// --- AST property helpers ---
bool Anchor::isAnchoredAtStart() const {
    return anchorType == Anchor::Start;
}

bool Repeat::containsZeroWidthRepeat() const {
    if (!Body) return false;
    if (Body->isZeroWidth()) return true;
    return Body->containsZeroWidthRepeat();
}

bool Concat::isAnchoredAtStart() const {
    if (BodyVec.empty()) return false;
    return BodyVec.front()->isAnchoredAtStart();
}

bool Concat::containsZeroWidthRepeat() const {
    for (const auto &b : BodyVec) {
        if (b->containsZeroWidthRepeat()) return true;
        // Also detect direct Repeat nodes whose body is zero-width
        // (we cannot safely dynamic_cast here in generator-land, so rely on
        // containsZeroWidthRepeat propagated by Repeat)
    }
    return false;
}

bool Alternative::isAnchoredAtStart() const {
    if (BodyVec.empty()) return false;
    for (const auto &b : BodyVec) {
        if (!b->isAnchoredAtStart()) return false;
    }
    return true;
}

bool Alternative::containsZeroWidthRepeat() const {
    for (const auto &b : BodyVec) {
        if (b->containsZeroWidthRepeat()) return true;
    }
    return false;
}

// New JIT compilation interface
bool CompileRegex(const std::string& pattern) {
    // Debug: show tokenization to help locate parser errors (only when debugging)
    RJDBG({
        RegexLexer tmp(pattern);
        errs() << "Lexer tokens for pattern: '" << pattern << "'\n";
        while (true) {
            auto t = tmp.get_next_token();
            if (t.type == RegexLexer::EOS) { errs() << "  <EOS>\n"; break; }
            errs() << "  token: " << t.type << " value:'" << t.value << "'\n";
        }
    });
    // If there is no per-compile context currently, this is a direct
    // invocation path (not via getOrCompile). In that case, clear
    // FunctionName so we always generate a fresh, unique name for the
    // function we will emit into the new module. This prevents duplicate
    // symbol errors when compiling multiple patterns in the same process.
    bool createdContext = false;
    bool directInvocation = (OwnedCompileContext == nullptr);
    if (directInvocation) FunctionName.clear();
    try {
        // If a per-compile context already exists (e.g. when called via
        // getOrCompile), reuse it. Otherwise create a fresh per-compile
        // context/module here for direct CompileRegex invocations.
        if (!OwnedCompileContext) {
            ThisModule.reset();
            OwnedCompileContext = std::make_unique<LLVMContext>();
            OwnedCompileBuilder = std::make_unique<IRBuilder<>>(*OwnedCompileContext);
            ContextPtr = OwnedCompileContext.get();
            BuilderPtr = OwnedCompileBuilder.get();
            RJDBG(errs() << "CompileRegex: created OwnedCompileContext=" << (void*)OwnedCompileContext.get() << "\n");
            ThisModule = std::make_unique<Module>("my_module", Context);
            if (JIT) ThisModule->setDataLayout(JIT->getDataLayout());
            createdContext = true;
        } else {
            RJDBG(errs() << "CompileRegex: reusing existing OwnedCompileContext=" << (void*)OwnedCompileContext.get() << " ThisModule=" << (void*)ThisModule.get() << "\n");
        }

        // Always generate a unique function name for this compile. This
        // prevents duplicate-symbol errors when compiling multiple modules
        // in the same process. The name includes a hash of the pattern and
        // a monotonically increasing id to avoid collisions.
        {
            uint64_t id = GlobalFnId.fetch_add(1);
            std::hash<std::string> hasher;
            auto h = hasher(pattern);
            FunctionName = "regjit_match_" + std::to_string(h) + "_" + std::to_string(id);
        }
        RJDBG(errs() << "CompileRegex: ThisModule=" << (void*)ThisModule.get() << " ContextPtr=" << (void*)ContextPtr << " FunctionName=" << FunctionName << "\n");
        RegexLexer lexer(pattern);
        RegexParser parser(lexer);
        auto ast = parser.parse();
        // Debug: dump AST structure
        RJDBG({
            std::function<void(Root*, int)> dump = [&](Root* r, int depth) -> void {
                std::string indent(depth*2, ' ');
                if (dynamic_cast<Func*>(r)) {
                    errs() << indent << "Func\n";
                } else if (dynamic_cast<Concat*>(r)) {
                    errs() << indent << "Concat\n";
                    Concat* c = static_cast<Concat*>(r);
                    for (auto &ch : c->BodyVec) dump(ch.get(), depth+1);
                    return;
                } else if (dynamic_cast<Match*>(r)) {
                    errs() << indent << "Match\n";
                } else if (dynamic_cast<Repeat*>(r)) {
                    errs() << indent << "Repeat\n";
                    Repeat* rep = static_cast<Repeat*>(r);
                    dump(rep->Body.get(), depth+1);
                    return;
                } else if (dynamic_cast<Anchor*>(r)) {
                    errs() << indent << "Anchor\n";
                } else if (dynamic_cast<CharClass*>(r)) {
                    errs() << indent << "CharClass\n";
                } else if (dynamic_cast<Alternative*>(r)) {
                    errs() << indent << "Alternative\n";
                    Alternative* a = static_cast<Alternative*>(r);
                    for (auto &ch : a->BodyVec) dump(ch.get(), depth+1);
                    return;
                } else if (dynamic_cast<Not*>(r)) {
                    errs() << indent << "Not\n";
                } else {
                    errs() << indent << "UnknownNode\n";
                }
            };
            errs() << "AST dump for pattern: '" << pattern << "'\n";
            // ast is a unique_ptr<Root>
            dump(ast.get(), 0);
        });
        auto func = std::make_unique<Func>(std::move(ast));
        func->CodeGen();
        Compile();

        // If we created a temporary per-compile context for this direct
        // invocation (not via getOrCompile), ownership will have been
        // transferred into the JIT by Compile(), so clear our local
        // pointers and defaults.
        if (createdContext) {
            // OwnedCompileContext has been moved into ThreadSafeContext by
            // Compile(); ensure the local unique_ptr is null and restore
            // global pointers.
            OwnedCompileContext.reset();
            OwnedCompileBuilder.reset();
            ContextPtr = &GlobalContext;
            BuilderPtr = &GlobalBuilder;
        }
        return true;
    } catch (const std::exception &e) {
        std::cerr << "CompileRegex failed for pattern '" << pattern << "': " << e.what() << "\n";
        // restore globals and drop any partially built module
        resetCompileState();
        return false;
    }
}
