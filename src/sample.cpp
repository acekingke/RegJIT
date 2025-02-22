#include "regjit.h"
#include <iostream>
int main(int argc, char** argv){
    Initialize();
    std::cout << "\nExample 1: a+b*\n";
    CompileRegex("a+b*");
    std::cout << "Test 'aaab' : " << (Execute("aaab")  ? "Match" : "No match") << "\n";
    std::cout << "Test 'abb'  : " << (Execute("abb")  ? "Match" : "No match") << "\n";
    std::cout << "Test 'b'    : " << (Execute("b") ? "Match" : "No match") << "\n";
}