#include "../src/regjit.h"
#include <iostream>
#include <string>

int main() {
    std::cout << "Simple anchor test...\n";
    
    // Test simple ^ anchor
    std::cout << "Testing ^a:\n";
    Initialize();
    CompileRegex(std::string("^a"));
    std::cout << "'a' -> ";
    int result = Execute("a");
    std::cout << "Result: " << result << std::endl;
    CleanUp();
    
    std::cout << "'b' -> ";
    Initialize();
    CompileRegex(std::string("^a"));
    std::cout << "'b' -> ";
    result = Execute("b");
    std::cout << "Result: " << result << std::endl;
    CleanUp();
    
    return 0;
}