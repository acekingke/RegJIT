   
   
#include "../src/regjit.h"
#include <iostream>
#include <string>
#include <cassert>

int main() {
    std::cout << "Testing anchor functionality...\n";

    // Case 1: ^ab*c$ on "abc" -> expected MATCH (1)
    Initialize();
    bool ok1 = CompileRegex(std::string("^ab*c$"));
    assert(ok1 && "CompileRegex failed for pattern ^ab*c$");
    std::cout << "'abc' -> ";
    int r1 = Execute("abc");
    std::cout << " (actual=" << r1 << ", expected=1)\n";
    assert(r1 == 1 && "Expected match for ^ab*c$ on 'abc'");
    CleanUp();

    // Case 2: ^a.c$ on "axcx" -> expected NO MATCH (0)
    Initialize();
    bool ok2 = CompileRegex(std::string("^a.c$"));
    assert(ok2 && "CompileRegex failed for pattern ^a.c$");
    std::cout << "'axcx' -> ";
    int r2 = Execute("axcx");
    std::cout << " (actual=" << r2 << ", expected=0)\n";
    assert(r2 == 0 && "Expected no match for ^a.c$ on 'axcx'");
    CleanUp();

    // Case 3: ^ab*c$ on "ac" -> expected MATCH (1)
    Initialize();
    bool ok3 = CompileRegex(std::string("^ab*c$"));
    assert(ok3 && "CompileRegex failed for pattern ^ab*c$");
    std::cout << "'ac' -> ";
    int r3 = Execute("ac");
    std::cout << " (actual=" << r3 << ", expected=1)\n";
    assert(r3 == 1 && "Expected match for ^ab*c$ on 'ac'");
    CleanUp();

    std::cout << "All test_wrong assertions passed.\n";
    return 0;
}
