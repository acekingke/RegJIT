#include "regjit.h"
#include <iostream>
#include <stdexcept>
#include "regjit_capi.h"

  ExitOnError ExitOnErr;
  LLVMContext Context;
  IRBuilder<> Builder(Context);
   std::unique_ptr<Module>ThisModule = nullptr;
   std::unique_ptr<llvm::orc::LLJIT> JIT;
   llvm::orc::ResourceTrackerSP RT;
   Function *MatchF;
   std::string FunctionName("match");
   // Compilation cache and helpers
   std::unordered_map<std::string, CompiledEntry> CompileCache;
   std::mutex CompileCacheMutex;
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
  ThisModule =  std::make_unique<Module>("my_module", Context);
  ThisModule->setDataLayout(JIT->getDataLayout());
  // Allow the JIT to resolve symbols from the host process (e.g. libc's strlen)
  JIT->getMainJITDylib().addGenerator(
      cantFail(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          JIT->getDataLayout().getGlobalPrefix())));

  // We compute string length inline inside Func::CodeGen to avoid depending
  // on host libc symbol resolution (e.g. renamed variants like "strlen.1").
  // Keep DynamicLibrarySearchGenerator in case other external symbols are
  // required by generated code.
  
  if (verifyModule(*ThisModule, &errs())) {
    errs() << "Error verifying module!\n";
    return;
  }

}

void ensureJITInitialized() {
  if (!JIT) {
    Initialize();
  }
}

// compile-or-get with cache. This uses CompileRegex which generates IR into
// the global ThisModule and calls Compile() to add it to the JIT. We hold
// CompileCacheMutex during compilation to avoid races and RT being overwritten.
CompiledEntry getOrCompile(const std::string &pattern) {
  // fast check
  {
    std::lock_guard<std::mutex> lk(CompileCacheMutex);
    auto it = CompileCache.find(pattern);
    if (it != CompileCache.end()) return it->second;
  }

  std::lock_guard<std::mutex> lk(CompileCacheMutex);
  // double-check
  auto it = CompileCache.find(pattern);
  if (it != CompileCache.end()) return it->second;

  ensureJITInitialized();

  uint64_t id = GlobalFnId.fetch_add(1);
  std::hash<std::string> hasher;
  auto h = hasher(pattern);
  FunctionName = "regjit_match_" + std::to_string(h) + "_" + std::to_string(id);

  // create fresh module for this compile
  ThisModule = std::make_unique<Module>("module_" + std::to_string(id), Context);
  ThisModule->setDataLayout(JIT->getDataLayout());

  // Call existing CompileRegex which will fill ThisModule and call Compile()
  if (!CompileRegex(pattern)) {
    throw std::runtime_error("compile failed");
  }

  // After Compile(), RT holds the ResourceTracker used for this module
  auto Sym = ExitOnErr(JIT->lookup(FunctionName));
  uint64_t addr = Sym.getValue();

  CompiledEntry e;
  e.Addr = addr;
  e.RT = RT; // RT set by Compile()
  e.FnName = FunctionName;

  CompileCache.emplace(pattern, e);
  return e;
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
  // Execute the last compiled function (CompileRegex sets up module named 'match')
  (void)pattern; // ignore pattern (compat with simple API)
  return Execute(cstr);
}

void regjit_unload(const char* pattern) {
  (void)pattern;
  CleanUp();
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

  outs() << "\nGenerated LLVM IR:\n";
  ThisModule->print(outs(), nullptr);

  // Optimize before code generation
  OptimizeModule(*ThisModule);
  
  // Add module to JIT
  RT = JIT->getMainJITDylib().createResourceTracker();
  auto TSCtx = std::make_unique<LLVMContext>();
  ThreadSafeContext SafeCtx(std::move(TSCtx));

  ExitOnErr(JIT->addIRModule(RT, 
    ThreadSafeModule(std::move(ThisModule), SafeCtx)));
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
  auto MatchSym = ExitOnErr(JIT->lookup("match"));
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

    // Done: store final length
    Builder.SetInsertPoint(StrlenDoneBB);
    Value *finalLen = Builder.CreateLoad(Builder.getInt32Ty(), lenIdx);
    Builder.CreateStore(finalLen, StrLenAlloca);

    // Load strlenVal from the alloca for use in the search loop
    Value *strlenVal = Builder.CreateLoad(Builder.getInt32Ty(), StrLenAlloca);
    
    // Create search loop basic blocks
    BasicBlock *LoopCheckBB = BasicBlock::Create(Context, "search_loop_check", MatchF);
    BasicBlock *LoopBodyBB = BasicBlock::Create(Context, "search_loop_body", MatchF);
    BasicBlock *LoopIncBB = BasicBlock::Create(Context, "search_loop_inc", MatchF);
    BasicBlock *ReturnFailBB = BasicBlock::Create(Context, "return_fail", MatchF);
    BasicBlock *ReturnSuccessBB = BasicBlock::Create(Context, "return_success", MatchF);
    
    // Start loop
    Builder.CreateBr(LoopCheckBB);
    
    // Search loop condition: for(curIdx=0; curIdx<=strlen; ++curIdx)
    Builder.SetInsertPoint(LoopCheckBB);
    Value *curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
    Value *cond = Builder.CreateICmpSLE(curIdx, strlenVal);
    Builder.CreateCondBr(cond, LoopBodyBB, ReturnFailBB);
    
    // Search loop body: try match at current curIdx
    Builder.SetInsertPoint(LoopBodyBB);
    // Set index value for match
    // (already set unless coming from inc, but safe to repeat)
    Builder.CreateStore(curIdx, Index);
    
    // Each search attempt gets its own AST success/fail blocks
    BasicBlock *TrySuccess = BasicBlock::Create(Context, "try_success", MatchF);
    BasicBlock *TryFail = BasicBlock::Create(Context, "try_fail", MatchF);
    Body->SetFailBlock(TryFail);
    Body->SetSuccessBlock(TrySuccess);
    Body->CodeGen();
    
    // On success: return 1
    Builder.SetInsertPoint(TrySuccess);
    Builder.CreateBr(ReturnSuccessBB);
    // On fail: increment and continue
    Builder.SetInsertPoint(TryFail);
    Builder.CreateBr(LoopIncBB);
    
    // Loop increment
    Builder.SetInsertPoint(LoopIncBB);
    Value *idxAfter = Builder.CreateAdd(curIdx, ConstantInt::get(Context, APInt(32, 1)));
    Builder.CreateStore(idxAfter, Index);
    Builder.CreateBr(LoopCheckBB);
    
    // Return success and fail blocks
    Builder.SetInsertPoint(ReturnSuccessBB);
    Builder.CreateRet(ConstantInt::get(Context, APInt(32, 1)));
    Builder.SetInsertPoint(ReturnFailBB);
    Builder.CreateRet(ConstantInt::get(Context, APInt(32, 0)));
    
    // Done
    return nullptr;
}
Value* Match::CodeGen() {
    // Load current index
    Value *CurI = Builder.CreateLoad(Builder.getInt32Ty(), Index);
    // Calculate s[i]
    Value *SChar = Builder.CreateGEP(Builder.getInt8Ty(), Arg0, {CurI});
    SChar = Builder.CreateLoad(Builder.getInt8Ty(), SChar);
    // Character comparison
    Value *Cmp = Builder.CreateICmpNE(SChar,  ConstantInt::get(Context, APInt(8, choice)));
 
    Builder.CreateCondBr(Cmp, GetFailBlock(), GetSuccessBlock());
    return nullptr;
}
Value* Concat::CodeGen() {
  if (BodyVec.empty()) return nullptr;
  
  // Pre-create transition blocks for each element (except last)
  std::vector<BasicBlock*> transitionBlocks;
  for (size_t i = 0; i < BodyVec.size() - 1; i++) {
    transitionBlocks.push_back(
      BasicBlock::Create(Context, "next", MatchF)
    );
  }
  
  // Set up first element
  BodyVec[0]->SetFailBlock(GetFailBlock());
  BodyVec[0]->SetSuccessBlock(
    BodyVec.size() > 1 ? transitionBlocks[0] : GetSuccessBlock()
  );
  BodyVec[0]->CodeGen();
  
  // Process middle elements with proper block chaining
  for (size_t i = 1; i < BodyVec.size(); i++) {
    Builder.SetInsertPoint(transitionBlocks[i-1]);
    
    // Increment index (except for anchors)
    if (dynamic_cast<Anchor*>(BodyVec[i-1].get()) == nullptr) {
      Value* CurI = Builder.CreateLoad(Builder.getInt32Ty(), Index);
      Value* NextI = Builder.CreateAdd(CurI, ConstantInt::get(Context, APInt(32, 1)));
      Builder.CreateStore(NextI, Index);
    }
    
    // Set up current element
    BodyVec[i]->SetFailBlock(GetFailBlock());
    BodyVec[i]->SetSuccessBlock(
      i < transitionBlocks.size() ? transitionBlocks[i] : GetSuccessBlock()
    );
    BodyVec[i]->CodeGen();
  }
  
  return nullptr;
}

void Concat::Append(std::unique_ptr<Root> r){
  BodyVec.push_back(std::move(r));
}

void Alternative::Append(std::unique_ptr<Root> r) {
  BodyVec.push_back(std::move(r));
}
Value* Alternative::CodeGen() {
   auto  It = BodyVec.begin();
   while (It != BodyVec.end()) {
     auto NextIt = It+1;
     (*It)->SetSuccessBlock(GetSuccessBlock()); // Match success
     if (NextIt != BodyVec.end()) {
       auto failBlock =  BasicBlock::Create(Context, "out", MatchF);
       (*It)->SetFailBlock(failBlock);

       (*It)->CodeGen();
       Builder.SetInsertPoint(failBlock);
     }else {
       (*It)->SetFailBlock(GetFailBlock());
       (*It)->CodeGen();
     }
     It++;
   }
   return nullptr;
}
Value* Not::CodeGen(){
  Body->SetFailBlock(GetSuccessBlock());
  Body->SetSuccessBlock(GetFailBlock());
  Body->CodeGen();
  return nullptr;
}

// --- PATCH: Handle quantifiers of zero-width (anchor-like) nodes ---
Value* Repeat::CodeGen() {
    // Detect anchor/zero-width nodes robustly (covers Anchor, lookaround, future types)
    bool bodyIsZeroWidth = Body->isZeroWidth();
    // If zero-width, only allow a single match (at most), per regex engine semantics.
    if (bodyIsZeroWidth) {
        /*
         * Zero-width quantifier logic (reference: PCRE, RE2, std::regex):
         * - {0,} ('*', zero or more): always matches (valid to match zero times at zero-width position)
         * - {1}: match only once at this position
         * - {>1}: cannot match (no position allows >1 consecutive zero-width match)
         * This covers all anchors, zero-width lookaround, etc.
         */
        if (minCount > 1) {
            // Impossible to match anchor more than once per position: instantly fail
            Builder.CreateBr(GetFailBlock());
            return nullptr;
        }
        // Accept if 0 is allowed (e.g. * quantifier or {0,1})
        if (minCount == 0) {
            Builder.CreateBr(GetSuccessBlock());
            return nullptr;
        }
        // min==1; Only one iteration is possible; match if anchor matches, else fail
        BasicBlock* bodyBlock = BasicBlock::Create(Context, "repeat_zero_width_one", MatchF);
        BasicBlock* finalBlock = BasicBlock::Create(Context, "repeat_zero_width_exit", MatchF);
        Builder.CreateBr(bodyBlock);
        Builder.SetInsertPoint(bodyBlock);
        Body->SetSuccessBlock(finalBlock);
        Body->SetFailBlock(GetFailBlock());
        Body->CodeGen();
        Builder.SetInsertPoint(finalBlock);
        Builder.CreateBr(GetSuccessBlock());
        return nullptr;
     }
     // 通用支持 minCount/maxCount/nonGreedy 的量词
     auto intTy = Builder.getInt32Ty();
    // 精确重复，直接循环min次
    if (minCount == maxCount && minCount > 0) {
        Value* counter = Builder.CreateAlloca(intTy);
        Builder.CreateStore(ConstantInt::get(Context, APInt(32, 0)), counter);
        BasicBlock* checkBlock = BasicBlock::Create(Context, "repeat_check_exact", MatchF);
        BasicBlock* bodyBlock = BasicBlock::Create(Context, "repeat_body_exact", MatchF);
        BasicBlock* exitBlock = BasicBlock::Create(Context, "repeat_exit_exact", MatchF);
        Builder.CreateBr(checkBlock);
        // count check
        Builder.SetInsertPoint(checkBlock);
        Value* cur = Builder.CreateLoad(intTy, counter);
        Value* cond = Builder.CreateICmpSLT(cur, ConstantInt::get(Context, APInt(32, minCount)));
        Builder.CreateCondBr(cond, bodyBlock, exitBlock);
        // repeat body
        Builder.SetInsertPoint(bodyBlock);
        Body->SetSuccessBlock(checkBlock);
        Body->SetFailBlock(GetFailBlock());
        Body->CodeGen();
        Value* after = Builder.CreateAdd(cur, ConstantInt::get(Context, APInt(32, 1)));
        Builder.CreateStore(after, counter);
        Builder.CreateBr(checkBlock);
        // exit
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
    // min loop体
    Builder.SetInsertPoint(incMin);
    Body->SetSuccessBlock(checkMin);
    Body->SetFailBlock(GetFailBlock());
    Body->CodeGen();
    Value* stepmin = Builder.CreateAdd(val, ConstantInt::get(Context, APInt(32,1)));
    Builder.CreateStore(stepmin, counter);
    Builder.CreateBr(checkMin);
    // min循环完后可进入max部分
    Builder.SetInsertPoint(checkMax);
    Value* val2 = Builder.CreateLoad(intTy, counter);
    Value* finished = maxR == -1 ? Builder.getFalse() : Builder.CreateICmpSGE(val2, ConstantInt::get(Context, APInt(32, maxR)));
    Builder.CreateCondBr(finished, exit, incMax);
    // max阶段：贪婪与非贪婪切分分支
    Builder.SetInsertPoint(incMax);
    if (nonGreedy) {
        // 非贪婪：尝试余下整体优先
        Builder.CreateBr(GetSuccessBlock());
        // 尝试更多匹配
        Body->SetSuccessBlock(checkMax);
        Body->SetFailBlock(exit);
        Body->CodeGen();
    } else {
        // 贪婪：优先消耗自身
        Body->SetSuccessBlock(checkMax);
        Body->SetFailBlock(exit);
        Body->CodeGen();
        // fail时才尝试整体success
        Builder.CreateBr(GetSuccessBlock());
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
        while (m_cur_token.type == RegexLexer::CHAR || 
               m_cur_token.type == RegexLexer::LPAREN) {
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

// New JIT compilation interface
bool CompileRegex(const std::string& pattern) {
    // Debug: show tokenization to help locate parser errors
    {
        RegexLexer tmp(pattern);
        std::cerr << "Lexer tokens for pattern: '" << pattern << "'\n";
        while (true) {
            auto t = tmp.get_next_token();
            if (t.type == RegexLexer::EOS) { std::cerr << "  <EOS>\n"; break; }
            std::cerr << "  token: " << t.type << " value:'" << t.value << "'\n";
        }
    }
    try {
        RegexLexer lexer(pattern);
        RegexParser parser(lexer);
        auto ast = parser.parse();
        auto func = std::make_unique<Func>(std::move(ast));
        func->CodeGen();
        Compile();
        return true;
    } catch (const std::exception &e) {
        std::cerr << "CompileRegex failed for pattern '" << pattern << "': " << e.what() << "\n";
        return false;
    }
}
