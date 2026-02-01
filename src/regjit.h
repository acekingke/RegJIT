#pragma once
#include <memory>
#include <set>
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
#include <future>

using namespace llvm;
using namespace llvm::orc;


  extern llvm::LLVMContext* ContextPtr;
  extern llvm::IRBuilder<>* BuilderPtr;
  extern   ExitOnError ExitOnErr;
  extern std::unique_ptr<llvm::orc::LLJIT> JIT;
  
  struct CompiledEntry {
    uint64_t Addr; // JIT absolute address
    llvm::orc::ResourceTrackerSP RT; // tracker to allow unloading
    std::string FnName; // generated function name
    size_t refCount = 0; // number of active users
    std::list<std::string>::iterator lruIt; // iterator into LRU list
  };

  extern std::unordered_map<std::string, CompiledEntry> CompileCache;
  extern std::mutex CompileCacheMutex;
  // Per-pattern in-flight compile coordination to avoid duplicate compilations
  struct InflightCompile {
    std::shared_ptr<std::promise<bool>> prom;
    std::shared_ptr<std::shared_future<bool>> fut;
  };
  extern std::unordered_map<std::string, InflightCompile> CompileInflight;
  extern std::atomic<uint64_t> GlobalFnId;
  extern std::string FunctionName; // current/last generated function name
  #include <list>
  extern size_t CacheMaxSize;
  extern std::list<std::string> CacheLRUList;

  // cache management
  CompiledEntry getOrCompile(const std::string &pattern);
  void releasePattern(const std::string &pattern);
  void evictIfNeeded();
  class Root {
    BasicBlock* failBlock = nullptr;
    BasicBlock* nextBlock = nullptr;
public:
    virtual ~Root() = default;
    virtual Value *CodeGen() = 0;
    // Returns true if this node is zero-width (e.g. anchor, lookaround, etc)
    virtual bool isZeroWidth() const { return false; }
    // Returns true if the subtree is guaranteed to only match at string start
    // (i.e. contains an explicit '^' anchor at the leftmost position and all
    // paths respect that). Conservative default: false.
    virtual bool isAnchoredAtStart() const { return false; }
    // Returns true if the subtree contains a Repeat whose body is zero-width
    // (e.g. repeating an anchor). Conservative default: false.
    virtual bool containsZeroWidthRepeat() const { return false; }
    // Returns the first literal character if the pattern starts with a literal
    // Returns -1 if not applicable (e.g., starts with anchor, char class, etc.)
    virtual int getFirstLiteralChar() const { return -1; }
    // Returns the literal prefix string (consecutive literal characters at start)
    // Returns empty string if pattern doesn't start with literals
    virtual std::string getLiteralPrefix() const { return ""; }
    // Returns true if this node is a pure literal (only Match nodes, no special chars)
    virtual bool isPureLiteral() const { return false; }
    // Returns the single character if this is a simple Match node, -1 otherwise
    // Used for optimizing patterns like a+, b*, etc.
    virtual int getSingleChar() const { return -1; }
    // Returns characters that MUST appear in any successful match
    // Used for pre-filtering optimization - quickly reject strings missing required chars
    virtual std::set<char> getRequiredChars() const { return {}; }
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
     char getChar() const { return choice; }
     Value* CodeGen() override;
     int getFirstLiteralChar() const override { return static_cast<unsigned char>(choice); }
     std::string getLiteralPrefix() const override { return std::string(1, choice); }
     bool isPureLiteral() const override { return true; }
     int getSingleChar() const override { return static_cast<unsigned char>(choice); }
     std::set<char> getRequiredChars() const override { return {choice}; }
      ~Match() override = default; 
  };
  class Concat: public Root{
    public:
    std::vector<std::unique_ptr<Root>> BodyVec;
    Concat(){}
    void Append(std::unique_ptr<Root> Body);
    Value* CodeGen() override;
    bool isAnchoredAtStart() const override;
    bool containsZeroWidthRepeat() const override;
    int getFirstLiteralChar() const override {
      // Skip zero-width elements (anchors) and return first literal
      for (const auto& child : BodyVec) {
        if (child->isZeroWidth()) continue;
        return child->getFirstLiteralChar();
      }
      return -1;
    }
    std::string getLiteralPrefix() const override {
      std::string result;
      for (const auto& child : BodyVec) {
        if (child->isZeroWidth()) continue;  // skip anchors
        if (!child->isPureLiteral()) break;  // stop at non-literal
        result += child->getLiteralPrefix();
      }
      return result;
    }
    bool isPureLiteral() const override {
      for (const auto& child : BodyVec) {
        if (child->isZeroWidth()) continue;
        if (!child->isPureLiteral()) return false;
      }
      return true;
    }
    std::set<char> getRequiredChars() const override {
      std::set<char> result;
      for (const auto& child : BodyVec) {
        auto childChars = child->getRequiredChars();
        result.insert(childChars.begin(), childChars.end());
      }
      return result;
    }
  };
  
  class Alternative: public Root{
  public:
    std::vector<std::unique_ptr<Root>> BodyVec;
    Alternative(){}
    void Append(std::unique_ptr<Root> Body);
    Value* CodeGen() override;
    bool isAnchoredAtStart() const override;
    bool containsZeroWidthRepeat() const override;
    std::set<char> getRequiredChars() const override {
      // For alternatives, only chars required by ALL branches are truly required
      if (BodyVec.empty()) return {};
      std::set<char> result = BodyVec[0]->getRequiredChars();
      for (size_t i = 1; i < BodyVec.size(); ++i) {
        auto branchChars = BodyVec[i]->getRequiredChars();
        std::set<char> intersection;
        for (char c : result) {
          if (branchChars.count(c)) intersection.insert(c);
        }
        result = std::move(intersection);
      }
      return result;
    }
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
    bool containsZeroWidthRepeat() const override;
    // Repeats are not considered anchored at start conservatively because
    // a repeat may wrap a zero-width anchor and change search semantics.
    bool isAnchoredAtStart() const override { return false; }
    std::set<char> getRequiredChars() const override {
      // If min is 0 (e.g., * or ?), the repeat is optional, so no chars required
      if (minCount == 0) return {};
      // If min > 0, the body's required chars are also required
      return Body->getRequiredChars();
    }
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
    bool isAnchoredAtStart() const override;
    bool containsZeroWidthRepeat() const override { return false; }
    ~Anchor() override = default;
  };
void Initialize();
void Compile();
bool CompileRegex(const std::string& pattern);
void ensureJITInitialized();
int Execute(const char* input); // execute last compiled function
int ExecutePattern(const std::string& pattern, const char* input); // compile-or-get then execute
void unloadPattern(const std::string& pattern);
void CleanUp();
