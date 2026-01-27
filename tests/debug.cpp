#include "../src/regjit.h"
#include <iostream>
#include <string>

int main() {
    Initialize();
    
    std::cout << "Testing simple character 'a' (no character class)...\n";
    
    // First test: simple character match
    CompileRegex(std::string("a"));
    std::cout << "'a' -> ";
    int result = Execute("a");
    std::cout << "Result: " << result << std::endl;
    
    CleanUp();
    
    std::cout << "\nTesting simple character class [abc]...\n";
    
    // Test character class
    CompileRegex(std::string("[abc]"));
    std::cout << "'a' -> ";
    result = Execute("a");
    std::cout << "Result: " << result << std::endl;
    
    CleanUp();
    
    std::cout << "Test completed!\n";
    
    return 0;
}