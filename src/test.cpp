#include "regjit.h"
void test1() {
  auto ConcatIns = std::make_unique<Concat>();
  ConcatIns->Append(std::make_unique<Match>('a'));
  ConcatIns->Append(std::make_unique<Match>('b'));
  ConcatIns->Append(std::make_unique<Match>('c'));
  auto FunBlock = std::make_unique<Func>(std::unique_ptr<Root>(ConcatIns.release()));
  FunBlock->CodeGen();
  Compile();
  Execute(const_cast<char*>("abc")); 
}

void test2() {
  auto AltInst = std::make_unique<Alternative>();
  AltInst->Append(std::make_unique<Match>('a'));
  AltInst->Append(std::make_unique<Match>('b'));
  AltInst->Append(std::make_unique<Match>('c'));
  auto FunBlock = std::make_unique<Func>(std::unique_ptr<Root>(AltInst.release()));
  FunBlock->CodeGen();
  Compile();
  Execute(const_cast<char*>("b")); 
  Execute(const_cast<char*>("a")); 
  Execute(const_cast<char*>("c"));
  Execute(const_cast<char*>("b")); 
}

void test4() {
  auto NotInst = std::make_unique<Not>(std::make_unique<Match>('a'));

  auto FunBlock = std::make_unique<Func>(std::unique_ptr<Root>(NotInst.release()));
  FunBlock->CodeGen();
  Compile();
  Execute(const_cast<char*>("a")); 

}
void test() {
  auto MatchIns = std::make_unique<Match>('c');
  auto FunBlock = std::make_unique<Func>(std::unique_ptr<Root>(MatchIns.release()));
  FunBlock->CodeGen();
  Compile();
  Execute(const_cast<char*>("b")); 
}

void test_repeat_star() {
  auto repeatBody = std::make_unique<Match>('a');
  auto repeatInst = std::make_unique<Repeat>(std::move(repeatBody), Repeat::Star);
  
  auto FunBlock = std::make_unique<Func>(std::move(repeatInst));
  FunBlock->CodeGen();
  Compile();
  
  // Should match 0+ 'a's
  Execute("");     // Should succeed (0 matches)
  Execute("a");    // Should succeed
  Execute("aaaa"); // Should succeed
  Execute("aab");  // Should fail (contains 'b')
}

void test_repeat_plus() {
  auto repeatBody = std::make_unique<Match>('b');
  auto repeatInst = std::make_unique<Repeat>(std::move(repeatBody), Repeat::Plus);
  
  auto FunBlock = std::make_unique<Func>(std::move(repeatInst));
  FunBlock->CodeGen();
  Compile();
  
  // Should match 1+ 'b's
  Execute("");     // Should fail
  Execute("b");    // Should succeed
  Execute("bbbb"); // Should succeed 
  Execute("bbc");  // Should fail
}

void test_repeat_exact() {
  auto repeatBody = std::make_unique<Match>('c');
  auto repeatInst = std::make_unique<Repeat>(std::move(repeatBody), 3);
  
  auto FunBlock = std::make_unique<Func>(std::move(repeatInst));
  FunBlock->CodeGen();
  Compile();
  
  // Should match exactly 3 'c's
  Execute("ccc");   // Should succeed
  Execute("cccc");  // Should fail (extra characters allowed)
  Execute("cccd");  // Should fail (extra characters allowed)
  Execute("ccdc");  // Should fail (wrong character)
}

int main(int argc, char** argv) {
  Initialize();
  test_repeat_exact();
  return 0;
}