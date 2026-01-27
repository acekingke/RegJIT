#include "regjit.h"

  ExitOnError ExitOnErr;
  LLVMContext Context;
  IRBuilder<> Builder(Context);
  std::unique_ptr<Module>ThisModule = nullptr;
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
   ThisModule =  std::make_unique<Module>("my_module", Context);
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    JIT = ExitOnErr(LLJITBuilder().create());
    ThisModule->setDataLayout(JIT->getDataLayout());
  
  if (verifyModule(*ThisModule, &errs())) {
    errs() << "Error verifying module!\n";
    return;
  }

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
      // Increment index only for non-anchor elements
      // Anchors are zero-width assertions and don't consume characters
      if (dynamic_cast<Anchor*>(It->get()) == nullptr) {
        Value *CurI = Builder.CreateLoad(Builder.getInt32Ty(), Index);
        Value *NextI = Builder.CreateAdd(CurI, ConstantInt::get(Context, APInt(32, 1)));
        Builder.CreateStore(NextI, Index);
      }
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
        Value* count = Builder.CreateLoad(Builder.getInt32Ty(), counter);
        Value* InitVal = Builder.CreateLoad(Builder.getInt32Ty(), Index);
        BasicBlock* checkBlock = BasicBlock::Create(Context, "repeat_check", MatchF);
        BasicBlock* loopBlock = BasicBlock::Create(Context, "repeat_loop", MatchF);
        BasicBlock* bodySuccess = BasicBlock::Create(Context, "repeat_success", MatchF);
        BasicBlock* exitBlock = BasicBlock::Create(Context, "repeat_exit", MatchF);

        Builder.CreateBr(checkBlock);
        
        // Check iteration count
        Builder.SetInsertPoint(checkBlock);
        PHINode *countPhi = Builder.CreatePHI(Type::getInt32Ty(Context), 2, "phi_node2");
        countPhi->addIncoming(count, origBlock);
        Value* cond = Builder.CreateICmpSLT(countPhi, 
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
        
        Value* newCount = Builder.CreateAdd(countPhi, ConstantInt::get(Context, APInt(32, 1)));
        countPhi->addIncoming(newCount, bodySuccess);
        //Builder.CreateStore(newCount, counter);
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
        DASH,    // -
        CARET,   // ^ (inside character class)
        CARET_ANCHOR, // ^ (line start anchor)
        DOLLAR,  // $ (line end anchor)
        DOT,     // .
        BACKSLASH, // \ (escape sequence start)
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
                case '-': next(); return {DASH, '-'};
                case '^': next(); return {CARET, '^'}; // Will be handled by parser based on context
                case '$': next(); return {DOLLAR, '$'};
                case '.': next(); return {DOT, '.'};
                case '\\': { // Handle escape characters
                    next();
                    char c = current();
                    if (is_end()) break;
                    next();
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
            m_cur_token = m_lexer.get_next_token();
            auto expr = parse_expr();
            if (m_cur_token.type != RegexLexer::RPAREN) {
                throw std::runtime_error("Mismatched parentheses");
            }
            m_cur_token = m_lexer.get_next_token();
            return expr;
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
        } else if (m_cur_token.type == RegexLexer::CHAR) {
            // Check for escape sequences \b, \B
            if (m_cur_token.value == 'b') {
                m_cur_token = m_lexer.get_next_token();
                return std::make_unique<Anchor>(Anchor::WordBoundary); // \b
            } else if (m_cur_token.value == 'B') {
                m_cur_token = m_lexer.get_next_token();
                return std::make_unique<Anchor>(Anchor::NonWordBoundary); // \B
            } else {
                char c = m_cur_token.value;
                m_cur_token = m_lexer.get_next_token();
                return std::make_unique<Match>(c);
            }
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
            Value* strLen = Builder.CreateCall(
                Function::Create(
                    FunctionType::get(Builder.getInt32Ty(), {Builder.getInt8Ty()->getPointerTo()}, false),
                    Function::ExternalLinkage,
                    "strlen",
                    ThisModule.get()
                ),
                {Arg0}
            );
            match = Builder.CreateICmpEQ(curIdx, strLen);
            break;
        }
        case WordBoundary: {
            // \b matches at word boundaries (transition between \w and \W)
            // Check if current position is at string boundaries
            Value* strLen = Builder.CreateCall(
                Function::Create(
                    FunctionType::get(Builder.getInt32Ty(), {Builder.getInt8Ty()->getPointerTo()}, false),
                    Function::ExternalLinkage,
                    "strlen",
                    ThisModule.get()
                ),
                {Arg0}
            );
            
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
            // For simplicity, we'll implement this as a separate check
            Value* strLen = Builder.CreateCall(
                Function::Create(
                    FunctionType::get(Builder.getInt32Ty(), {Builder.getInt8Ty()->getPointerTo()}, false),
                    Function::ExternalLinkage,
                    "strlen",
                    ThisModule.get()
                ),
                {Arg0}
            );
            
            Value* atStart = Builder.CreateICmpEQ(curIdx, 
                ConstantInt::get(Context, APInt(32, 0)));
            Value* atEnd = Builder.CreateICmpEQ(curIdx, strLen);
            Value* atBoundary = Builder.CreateOr(atStart, atEnd);
            
            // If at boundary, this is not a non-word boundary
            BasicBlock* checkTransition = BasicBlock::Create(Context, "check_nonword_transition", MatchF);
            BasicBlock* endCheck = BasicBlock::Create(Context, "end_nonword_check", MatchF);
            
            Builder.CreateCondBr(atBoundary, endCheck, checkTransition);
            
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
            result->addIncoming(ConstantInt::get(Context, APInt(1, 0)), atBoundary ? Builder.GetInsertBlock() : Builder.GetInsertBlock());
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
void CompileRegex(const std::string& pattern) {
    RegexLexer lexer(pattern);
    RegexParser parser(lexer);
    auto ast = parser.parse();
    auto func = std::make_unique<Func>(std::move(ast));
    func->CodeGen();
    Compile();
}

