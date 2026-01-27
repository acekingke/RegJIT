#include "../src/regjit.h"
#include <iostream>
#include <string>

int main() {
    Initialize();
    
    std::cout << "Testing character class functionality...\n";
    
    // Test simple character class [abc]
    std::cout << "\nTesting [abc]:\n";
    CompileRegex(std::string("[abc]"));
    std::cout << "'a' -> "; Execute("a");  // Should succeed
    CleanUp();
     Initialize();
    CompileRegex(std::string("[abc]"));
    std::cout << "'b' -> "; Execute("b");  // Should succeed
    CleanUp();
     Initialize();
    CompileRegex(std::string("[abc]"));
    std::cout << "'d' -> "; Execute("d");  // Should fail
    CleanUp();
    
    return 0;
}