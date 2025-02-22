#include "regjit.h"

  ExitOnError ExitOnErr;
  LLVMContext Context;
  IRBuilder<> Builder(Context);
  std::unique_ptr<Module>ThisModule = std::make_unique<Module>("my_module", Context);;
  std::unique_ptr<llvm::orc::LLJIT> JIT;
  llvm::orc::ResourceTrackerSP RT;
  Function *MatchF;
  const std::string FunctionName("match");
  const std::string FunArgName("Arg0");
  const std::string TrueBlockName("TrueBlock");
  const std::string FalseBlockName("FalseBlock");
  Value *Index;
  Value *Arg0;
  

void Initialize() {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    JIT = ExitOnErr(LLJITBuilder().create());
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
 if (verifyModule(*ThisModule, &errs())) {
    errs() << "Error verifying module!\n";
    return;
  }

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
  ExitOnErr(RT->remove());
}
int Execute(const char* input) {
  auto MatchSym = ExitOnErr(JIT->lookup("match"));
  auto Func = (int (*)(const char*))MatchSym.getValue();

  int ResultCode = Func(input);
  outs() << "\nProgram exited with code: " << ResultCode << "\n";
  return ResultCode;
}

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

    BasicBlock *EntryBB = BasicBlock::Create(Context, "entry", MatchF);
    Builder.SetInsertPoint(EntryBB);
    Index = Builder.CreateAlloca(Builder.getInt32Ty());
    Builder.CreateStore(ConstantInt::get(Context, APInt(32, 0)), Index);
    BasicBlock *TrueBlock = BasicBlock::Create(Context, TrueBlockName, MatchF);
  BasicBlock *FalseBlock = BasicBlock::Create(Context, FalseBlockName, MatchF);
  Body->SetFailBlock(FalseBlock);
  Body->SetSuccessBlock(TrueBlock);
  Body->CodeGen(); 
  
  // Add end-of-string check
  Builder.SetInsertPoint(TrueBlock);
  // Load current index
  Value *CurIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
  // CurIdx++
  Value *NextIdx = Builder.CreateAdd(CurIdx, ConstantInt::get(Context, APInt(32, 1)));
  // Get string length using strlen
  Value *StrLen = Builder.CreateCall(
      ThisModule->getOrInsertFunction("strlen", 
          FunctionType::get(Builder.getInt32Ty(), {PointerType::get(Builder.getInt8Ty(), 0)}, false)),
      {Arg0});
  // Compare index with string length
  Value *AtEnd = Builder.CreateICmpEQ(NextIdx, StrLen);
  BasicBlock *RealTrueBlock = BasicBlock::Create(Context, "real_true", MatchF);
  Builder.CreateCondBr(AtEnd, RealTrueBlock, FalseBlock);
  
  // Update final return blocks
 
  Builder.SetInsertPoint(RealTrueBlock);
  Builder.CreateRet(ConstantInt::get(Context, APInt(32, 1)));
  Builder.SetInsertPoint(FalseBlock);
  Builder.CreateRet(ConstantInt::get(Context, APInt(32, 0)));
  

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
  auto  It = BodyVec.begin();
  while (It != BodyVec.end()) {
    auto NextIt = It+1;
    BasicBlock* SuccessBlock;
    (*It)->SetFailBlock(GetFailBlock());
    if (NextIt != BodyVec.end()) {
      SuccessBlock =  BasicBlock::Create(Context, "next", MatchF);
      (*It)->SetSuccessBlock(SuccessBlock);
      (*It)->CodeGen();
      Builder.SetInsertPoint(SuccessBlock);
      // Increment index
      Value *CurI = Builder.CreateLoad(Builder.getInt32Ty(), Index);
      Value *NextI = Builder.CreateAdd(CurI, ConstantInt::get(Context, APInt(32, 1)));
      Builder.CreateStore(NextI, Index);
    }else {
      (*It)->SetSuccessBlock(GetSuccessBlock());
      (*It)->CodeGen();
    }
    It++;
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
    BasicBlock* SuccessBlock;
    (*It)->SetSuccessBlock(GetSuccessBlock()); // Match success
    if (NextIt != BodyVec.end()) {
      auto failBlock =  BasicBlock::Create(Context, "out", MatchF);
      (*It)->SetFailBlock(failBlock);

      (*It)->CodeGen();
      Builder.SetInsertPoint(failBlock);
      Value *CurI = Builder.CreateLoad(Builder.getInt32Ty(), Index);
      Value *NextI = Builder.CreateAdd(CurI, ConstantInt::get(Context, APInt(32, 1)));
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

Value* Repeat::CodeGen() {
    BasicBlock* origBlock = Builder.GetInsertBlock();
    
    if(times == Star) { // 0 or more times
        BasicBlock* loopBlock = BasicBlock::Create(Context, "repeat_loop", MatchF);
        BasicBlock* bodySuccess = BasicBlock::Create(Context, "repeat_success", MatchF);
        BasicBlock* exitBlock = BasicBlock::Create(Context, "repeat_exit", MatchF);

        Builder.CreateBr(loopBlock);
        
        // Loop block
        Builder.SetInsertPoint(loopBlock);
        Body->SetSuccessBlock(bodySuccess);
        Body->SetFailBlock(exitBlock);
        Body->CodeGen();

        // Body success - increment index and loop
        Builder.SetInsertPoint(bodySuccess);
        Value* curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
        Value* newIdx = Builder.CreateAdd(curIdx, ConstantInt::get(Context, APInt(32, 1)));
        Builder.CreateStore(newIdx, Index);
        Builder.CreateBr(loopBlock);

        // Exit block - continue to success
        Builder.SetInsertPoint(exitBlock);
        // Rollback index on failure
        curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
        newIdx = Builder.CreateSub(curIdx, ConstantInt::get(Context, APInt(32, 1)));
        Builder.CreateStore(newIdx, Index);
        Builder.CreateBr(GetSuccessBlock());
    }
    else if(times == Plus) { // 1 or more times
        BasicBlock* firstCheck = BasicBlock::Create(Context, "repeat_first", MatchF);
        BasicBlock* loopBlock = BasicBlock::Create(Context, "repeat_loop", MatchF);
        BasicBlock* bodySuccess = BasicBlock::Create(Context, "repeat_success", MatchF);
        BasicBlock* exitBlock = BasicBlock::Create(Context, "repeat_exit", MatchF);

        Builder.CreateBr(firstCheck);
        
        // First mandatory check
        Builder.SetInsertPoint(firstCheck);
        Body->SetSuccessBlock(bodySuccess);
        Body->SetFailBlock(GetFailBlock()); // Fail if first match fails
        Body->CodeGen();

        // First success - increment index and enter loop
        Builder.SetInsertPoint(bodySuccess);
        Value* curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
        Value* newIdx = Builder.CreateAdd(curIdx, ConstantInt::get(Context, APInt(32, 1)));
        Builder.CreateStore(newIdx, Index);
        Builder.CreateBr(loopBlock);

        // Loop block for additional matches
        Builder.SetInsertPoint(loopBlock);
        Body->SetSuccessBlock(bodySuccess); // Continue looping on success
        Body->SetFailBlock(exitBlock);       // Exit on failure
        Body->CodeGen();

        // Exit block
        Builder.SetInsertPoint(exitBlock);
        // Rollback index on failure
        curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
        newIdx = Builder.CreateSub(curIdx, ConstantInt::get(Context, APInt(32, 1)));
        Builder.CreateStore(newIdx, Index);
        Builder.CreateBr(GetSuccessBlock());
    }
    else if(times > 0) { // Exact count
        Value* counter = Builder.CreateAlloca(Builder.getInt32Ty());
        Builder.CreateStore(ConstantInt::get(Context, APInt(32, 0)), counter);
        
        BasicBlock* checkBlock = BasicBlock::Create(Context, "repeat_check", MatchF);
        BasicBlock* loopBlock = BasicBlock::Create(Context, "repeat_loop", MatchF);
        BasicBlock* bodySuccess = BasicBlock::Create(Context, "repeat_success", MatchF);
        BasicBlock* exitBlock = BasicBlock::Create(Context, "repeat_exit", MatchF);

        Builder.CreateBr(checkBlock);
        
        // Check iteration count
        Builder.SetInsertPoint(checkBlock);
        Value* count = Builder.CreateLoad(Builder.getInt32Ty(), counter);
        Value* cond = Builder.CreateICmpSLT(count, 
            ConstantInt::get(Context, APInt(32, times)));
        Builder.CreateCondBr(cond, loopBlock, exitBlock);

        // Loop block
        Builder.SetInsertPoint(loopBlock);
        Body->SetSuccessBlock(bodySuccess);
        Body->SetFailBlock(GetFailBlock()); // Fail if any iteration fails
        Body->CodeGen();

        // Body success - increment both counters
        Builder.SetInsertPoint(bodySuccess);
        Value* curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
        Value* newIdx = Builder.CreateAdd(curIdx, ConstantInt::get(Context, APInt(32, 1)));
        Builder.CreateStore(newIdx, Index);
        
        Value* newCount = Builder.CreateAdd(count, ConstantInt::get(Context, APInt(32, 1)));
        Builder.CreateStore(newCount, counter);
        Builder.CreateBr(checkBlock);

        // Exit block
        Builder.SetInsertPoint(exitBlock);
        // Rollback index on failure
        curIdx = Builder.CreateLoad(Builder.getInt32Ty(), Index);
        newIdx = Builder.CreateSub(curIdx, ConstantInt::get(Context, APInt(32, 1)));
        Builder.CreateStore(newIdx, Index);
        Builder.CreateBr(GetSuccessBlock());
    }
    
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

    enum TokenType {
        CHAR,
        STAR,   // *
        PLUS,   // +
        QMARK,  // ?
        PIPE,   // |
        LPAREN, // (
        RPAREN, // )
        EOS     // End of string
    };

    struct Token {
        TokenType type;
        char value;
    };

    Token get_next_token() {
        while (m_cur_char != '\0') {
            switch (m_cur_char) {
                case '*': next(); return {STAR, '*'};
                case '+': next(); return {PLUS, '+'};
                case '?': next(); return {QMARK, '?'};
                case '|': next(); return {PIPE, '|'};
                case '(': next(); return {LPAREN, '('};
                case ')': next(); return {RPAREN, ')'};
                case '\\': { // Handle escape characters
                    next();
                    char c = current();
                    next();
                    return {CHAR, c};
                }
                default: {
                    char c = current();
                    next();
                    return {CHAR, c};
                }
            }
        }
        return {EOS, '\0'};
    }
};

// New syntax analyzer
class RegexParser {
    RegexLexer& m_lexer;
    RegexLexer::Token m_cur_token;

    std::unique_ptr<Root> parse_element() {
        if (m_cur_token.type == RegexLexer::LPAREN) {
            m_cur_token = m_lexer.get_next_token();
            auto expr = parse_expr();
            if (m_cur_token.type != RegexLexer::RPAREN) {
                throw std::runtime_error("Mismatched parentheses");
            }
            m_cur_token = m_lexer.get_next_token();
            return expr;
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
                    node = std::make_unique<Repeat>(std::move(node), Repeat::Star);
                    break;
                }
                case RegexLexer::PLUS: {
                    m_cur_token = m_lexer.get_next_token();
                    node = std::make_unique<Repeat>(std::move(node), Repeat::Plus);
                    break;
                }
                case RegexLexer::QMARK: {
                    m_cur_token = m_lexer.get_next_token();
                    // Implement ? syntax (0 or 1 times)
                    auto alt = std::make_unique<Alternative>();
                    alt->Append(std::make_unique<Match>('\0')); // Empty match
                    alt->Append(std::move(node));
                    node = std::move(alt);
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

// New JIT compilation interface
void CompileRegex(const std::string& pattern) {
    RegexLexer lexer(pattern);
    RegexParser parser(lexer);
    auto ast = parser.parse();
    auto func = std::make_unique<Func>(std::move(ast));
    func->CodeGen();
    Compile();
}

