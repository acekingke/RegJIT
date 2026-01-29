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
#include <llvm/Support/Process.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Target/TargetMachine.h>

#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <string>

using namespace llvm;
using namespace llvm::orc;


  extern llvm::LLVMContext Context;
  extern llvm::IRBuilder<> Builder;
  extern   ExitOnError ExitOnErr;
  extern std::unique_ptr<llvm::orc::LLJIT> JIT;
  
  struct CompiledEntry {
    uint64_t Addr; // JIT absolute address
    llvm::orc::ResourceTrackerSP RT; // tracker to allow unloading
    std::string FnName; // generated function name
  };

  extern std::unordered_map<std::string, CompiledEntry> CompileCache;
  extern std::mutex CompileCacheMutex;
  extern std::atomic<uint64_t> GlobalFnId;
  extern std::string FunctionName; // current/last generated function name
class Root {
    BasicBlock* failBlock;
    BasicBlock* nextBlock;
public:
    virtual ~Root() = default;
    virtual Value *CodeGen() = 0;
    // Returns true if this node is zero-width (e.g. anchor, lookaround, etc)
    virtual bool isZeroWidth() const { return false; }
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
    std::unique_ptr<Root> Body;
    int minCount;
    int maxCount;
    bool nonGreedy;
    // 通用构造函数——支持所有量词
    Repeat(std::unique_ptr<Root> b, int min, int max, bool nongreedy = false)
      : Body(std::move(b)), minCount(min), maxCount(max), nonGreedy(nongreedy){}

    // 兼容旧的 * 与 + 构造方式
    static std::unique_ptr<Repeat> makeStar(std::unique_ptr<Root> b, bool nongreedy=false) {
      return std::make_unique<Repeat>(std::move(b), 0, -1, nongreedy);
    }
    static std::unique_ptr<Repeat> makePlus(std::unique_ptr<Root> b, bool nongreedy=false) {
      return std::make_unique<Repeat>(std::move(b), 1, -1, nongreedy);
    }
    static std::unique_ptr<Repeat> makeExact(std::unique_ptr<Root> b, int n, bool nongreedy=false) {
      return std::make_unique<Repeat>(std::move(b), n, n, nongreedy);
    }
    static std::unique_ptr<Repeat> makeRange(std::unique_ptr<Root> b, int min, int max, bool nongreedy=false) {
      return std::make_unique<Repeat>(std::move(b), min, max, nongreedy);
    }
    Value* CodeGen() override;
    ~Repeat() override = default;
  };


  // Character class
  class CharClass: public Root {
    public:
    struct CharRange {
        char start;
        char end;
        bool included;
        
        CharRange(char s, char e, bool inc = true) : start(s), end(e), included(inc) {}
    };
    
    private:
    std::vector<CharRange> ranges;
    bool negated;
    bool dotClass;
    
    public:
    explicit CharClass(bool neg = false, bool dot = false) : negated(neg), dotClass(dot) {}
    
    void addRange(char start, char end, bool included = true) {
        ranges.emplace_back(start, end, included);
    }
    
    void addChar(char c, bool included = true) {
        ranges.emplace_back(c, c, included);
    }
    
    bool isNegated() const { return negated; }
    bool isDotClass() const { return dotClass; }
    const std::vector<CharRange>& getRanges() const { return ranges; }
    
    Value* CodeGen() override;
    ~CharClass() override = default;
  };

  // Anchor class for ^, $, \b, \B
  class Anchor: public Root {
public:
    enum AnchorType {
        Start,          // ^ - line start
        End,            // $ - line end
        WordBoundary,   // \b - word boundary
        NonWordBoundary // \B - non-word boundary
    };
private:
    AnchorType anchorType;
public:
    explicit Anchor(AnchorType type) : anchorType(type) {}
    AnchorType getType() const { return anchorType; }
    Value* CodeGen() override;
    bool isZeroWidth() const override { return true; }
    ~Anchor() override = default;
};
void Initialize();
void Compile();
bool CompileRegex(const std::string& pattern);
void ensureJITInitialized();
bool CompileRegex(const std::string& pattern);
int Execute(const char* input); // execute last compiled function
int ExecutePattern(const std::string& pattern, const char* input); // compile-or-get then execute
void unloadPattern(const std::string& pattern);
void CleanUp();
