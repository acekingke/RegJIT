#include "regjit.h"
#include <iostream>
#include <stdexcept>
#include "regjit_capi.h"
#include <future>
#include <thread>
#include <chrono>
#include "llvm/IR/Verifier.h"

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
    fprintf(stderr, "regjit_trace: %s idx=%d cnt=%d\n", tag, idx, cnt);
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
      fprintf(stderr, "getOrCompile: compiling pattern='%s' -> FunctionName='%s'\n", pattern.c_str(), FunctionName.c_str());

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
        // restore global pointers on failure
        ContextPtr = &GlobalContext;
        BuilderPtr = &GlobalBuilder;
        OwnedCompileBuilder.reset();
        OwnedCompileContext.reset();
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
  // Dump the module to a temporary file for debugging (always) so we can
  // inspect IR that triggers optimizer crashes.
  {
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
  }
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
  // Print the function lookup info so we can correlate runtime calls with
  // the IR/module dumps and verify whether naming changes propagated.
  fprintf(stderr, "Execute(): FunctionName='%s' lookupName='%s'\n", FunctionName.c_str(), lookupName.c_str());
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
    
    // Compute string length inline (avoid external strlen calls which can
    // produce renamed declarations like "strlen.1" across module boundaries
    // and complicate JIT symbol resolution on some platforms).
    StrLenAlloca = Builder.CreateAlloca(Builder.getInt32Ty());
    // temp index for strlen loop
    Value *lenIdx = Builder.CreateAlloca(Builder.getInt32Ty());
    Builder.CreateStore(ConstantInt::get(Context, APInt(32, 0)), lenIdx);

    BasicBlock *StrlenCondBB = BasicBlock::Create(Context, "strlen_cond", MatchF);
    BasicBlock *StrlenBodyBB = BasicBlock::Create(Context, "strlen_body", MatchF);
    // Create the post-strlen continuation block early so we can guarantee
    // strlen_done branches to it and thus always has a terminator.
    BasicBlock *PostStrlenBB = BasicBlock::Create(Context, "post_strlen", MatchF);
    BasicBlock *StrlenDoneBB = BasicBlock::Create(Context, "strlen_done", MatchF);

    // Jump to condition
    Builder.CreateBr(StrlenCondBB);

    // Condition: load char at cur index and test null
    Builder.SetInsertPoint(StrlenCondBB);
    Value *curLen = Builder.CreateLoad(Builder.getInt32Ty(), lenIdx);
    Value *charPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, {curLen});
    Value *ch = Builder.CreateLoad(Builder.getInt8Ty(), charPtr);
    Value *isNull = Builder.CreateICmpEQ(ch, ConstantInt::get(Context, APInt(8, 0)));
    Builder.CreateCondBr(isNull, StrlenDoneBB, StrlenBodyBB);

    // Body: increment index and jump back to condition
    Builder.SetInsertPoint(StrlenBodyBB);
    Value *nextLen = Builder.CreateAdd(curLen, ConstantInt::get(Context, APInt(32, 1)));
    Builder.CreateStore(nextLen, lenIdx);
    Builder.CreateBr(StrlenCondBB);

    // Done: store final length and jump to post-strlen block
    Builder.SetInsertPoint(StrlenDoneBB);
    Value *finalLen = Builder.CreateLoad(Builder.getInt32Ty(), lenIdx);
    Builder.CreateStore(finalLen, StrLenAlloca);
    Builder.CreateBr(PostStrlenBB);
    // Ensure strlen_done has a terminator (defensive).
    ensureBlockHasTerminator(StrlenDoneBB, PostStrlenBB);
    
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

        // Create search loop basic blocks
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
        
        // Before calling CodeGen on the body, we must reset the index for each
        // attempt in the search loop. The index is stored in the 'Index' alloca.
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
        
        // Regular (non-zero-width) body handling
        BasicBlock* checkBlock = BasicBlock::Create(Context, "repeat_check", MatchF);
        BasicBlock* bodyBlock = BasicBlock::Create(Context, "repeat_body", MatchF);
        BasicBlock* exitBlock = BasicBlock::Create(Context, "repeat_exit", MatchF);

        if (!isStar) { // Plus: must match at least once
            Body->SetSuccessBlock(checkBlock);
            Body->SetFailBlock(GetFailBlock());
            Body->CodeGen();
        } else { // Star: can match zero times
            Builder.CreateBr(checkBlock);
        }

        Builder.SetInsertPoint(checkBlock);

        if (nonGreedy) {
            // Non-greedy: first try to exit, then try to match
            Builder.CreateCondBr(Builder.getTrue(), GetSuccessBlock(), bodyBlock);
        } else {
            // Greedy: first try to match, then exit on failure
            Body->SetSuccessBlock(checkBlock); // Loop back on success
            Body->SetFailBlock(exitBlock);   // Exit on failure
            Builder.CreateBr(bodyBlock);
        }

        Builder.SetInsertPoint(bodyBlock);
        Body->CodeGen();
        if (Builder.GetInsertBlock()->getTerminator() == nullptr) {
          if (nonGreedy) Builder.CreateBr(checkBlock);
          else Builder.CreateBr(exitBlock);
        }

        Builder.SetInsertPoint(exitBlock);
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
                    // Return special tokens for \b and \B
                    if (c == 'b') return {WORD_BOUNDARY, 'b'};
                    if (c == 'B') return {NON_WORD_BOUNDARY, 'B'};
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
    // includes CHAR, group '(', dot '.', character class '[' and anchor
    // tokens (^, $, \b, \B). Previous implementation only checked for
    // CHAR or LPAREN which caused trailing anchors (like '$') to be left
    // unconsumed and dropped from the AST.
    while (m_cur_token.type == RegexLexer::CHAR ||
           m_cur_token.type == RegexLexer::LPAREN ||
           m_cur_token.type == RegexLexer::DOT ||
           m_cur_token.type == RegexLexer::LBRACKET ||
           m_cur_token.type == RegexLexer::CARET ||
           m_cur_token.type == RegexLexer::DOLLAR ||
           m_cur_token.type == RegexLexer::WORD_BOUNDARY ||
           m_cur_token.type == RegexLexer::NON_WORD_BOUNDARY) {
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
    // Load current character from input
    Value* curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
    Value* charPtr = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, curIdx);
    Value* currentChar = Builder.CreateLoad(Builder.getInt8Ty(), charPtr);
    currentChar = Builder.CreateIntCast(currentChar, Builder.getInt32Ty(), false);
    
    BasicBlock* matchBlock = BasicBlock::Create(Context, "charclass_match", MatchF);
    BasicBlock* nomatchBlock = BasicBlock::Create(Context, "charclass_nomatch", MatchF);
    
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
        // restore globals if we created per-compile context
        if (createdContext) {
            OwnedCompileBuilder.reset();
            OwnedCompileContext.reset();
            ContextPtr = &GlobalContext;
            BuilderPtr = &GlobalBuilder;
        }
        return false;
    }
}
