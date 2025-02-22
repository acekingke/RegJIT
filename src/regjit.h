#pragma once
#include <memory>
#include <llvm/ADT/APInt.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Verifier.h>

#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Passes/OptimizationLevel.h>
using namespace llvm;
using namespace llvm::orc;


  extern llvm::LLVMContext Context;
  extern llvm::IRBuilder<> Builder;
  extern   ExitOnError ExitOnErr;
  extern std::unique_ptr<llvm::orc::LLJIT> JIT;
class Root {
    BasicBlock* failBlock;
    BasicBlock* nextBlock;
    public:
    virtual ~Root() = default;
    virtual Value *CodeGen() = 0;
    void SetFailBlock(BasicBlock *b) {
      failBlock = b;
    }
    BasicBlock* GetFailBlock() {
      return failBlock;
    }
    void SetSuccessBlock(BasicBlock *b) {
      nextBlock = b;
    }
    BasicBlock* GetSuccessBlock() {
      return nextBlock;
    }
  };

  class Func: public Root {
    public:
      std::unique_ptr<Root> Body;
      explicit Func(std::unique_ptr<Root> b):Body(std::move(b)){}
      Value *CodeGen() override;
       ~Func() override = default; // 显式声明析构函数
  };

  class Match : public Root{
    char choice;

    public:
     explicit Match(char x) :choice(x){}
      
     Value* CodeGen() override;
      ~Match() override = default; 
  };
  class Concat: public Root{
    public:
    std::vector<std::unique_ptr<Root>> BodyVec;
    Concat(){}
    void Append(std::unique_ptr<Root> Body);
    Value* CodeGen() override;
  };
  
  class Alternative: public Root{
  public:
    std::vector<std::unique_ptr<Root>> BodyVec;
    Alternative(){}
    void Append(std::unique_ptr<Root> Body);
    Value* CodeGen() override;
  };
  // not operator
  class Not: public Root {
    public:
    std::unique_ptr<Root> Body;
    explicit Not(std::unique_ptr<Root> b):Body(std::move(b)){}
    Value* CodeGen() override;
    ~Not()override = default; 
  };
  // Repeat
  class Repeat: public Root{
    public:
    enum TimeType{
      Star = 0,
      Plus = -1
    };
    std::unique_ptr<Root> Body;
    int times;
    explicit Repeat(std::unique_ptr<Root> b, int t):Body(std::move(b)), times(t){}
    Value* CodeGen() override;
    ~Repeat() override = default;
  };
  void Initialize();
  void Compile();
  void CompileRegex(const std::string& pattern);
  int Execute(const char* input);




