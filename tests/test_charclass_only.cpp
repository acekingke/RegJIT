#include "../src/regjit.h"
#include <iostream>
#include <string>

int main() {
    std::cout << "Testing character class only...\n";
    
    Initialize();
    
    // Test character class only
    CompileRegex(std::string("[abc]"));
    std::cout << "'a' -> ";
    int result = Execute("a");
    std::cout << "Result: " << result << std::endl;
    
    CleanUp();
    
    std::cout << "Test completed successfully!\n";
    
    return 0;
}