#include "../src/regjit.h"
#include <iostream>
#include <string>

int main() {
    Initialize();
    
    std::cout << "Testing multiple Initialize/CleanUp cycles...\n";
    
    // First cycle
    std::cout << "\nCycle 1:\n";
    CompileRegex(std::string("a"));
    std::cout << "'a' -> "; Execute("a");
    CleanUp();
    
    // Second cycle - should work
    std::cout << "\nCycle 2:\n";
    Initialize();  // Re-initialize
    CompileRegex(std::string("b"));
    std::cout << "'b' -> "; Execute("b");
    CleanUp();
    
    // Third cycle - should work
    std::cout << "\nCycle 3:\n";
    Initialize();  // Re-initialize
    CompileRegex(std::string("[abc]"));
    std::cout << "'c' -> "; Execute("c");
    CleanUp();
    
    // Multiple CleanUp calls - should not crash
    std::cout << "\nTesting multiple CleanUp calls:\n";
    CleanUp();  // This should not crash
    CleanUp();  // This should not crash
    CleanUp();  // This should not crash
    
    std::cout << "Test completed successfully!\n";
    
    return 0;
}