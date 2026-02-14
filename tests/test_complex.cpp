#include "regjit.h"
#include <iostream>
#include <string>
#include <chrono>

void test_pattern(const std::string& name, const std::string& pattern, const std::string& input) {
    std::cout << "Testing: " << name << " pattern='" << pattern << "'" << std::endl;
    
    Initialize();
    
    auto start = std::chrono::high_resolution_clock::now();
    bool compiled = CompileRegex(pattern);
    auto end = std::chrono::high_resolution_clock::now();
    
    if (!compiled) {
        std::cerr << "  Failed to compile!" << std::endl;
        CleanUp();
        return;
    }
    
    std::cout << "  Compiled in " 
              << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() 
              << "ms" << std::endl;
    
    start = std::chrono::high_resolution_clock::now();
    int result = Execute(input.c_str());
    end = std::chrono::high_resolution_clock::now();
    
    std::cout << "  Executed in " 
              << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() 
              << "us, result=" << result << std::endl;
    
    CleanUp();
}

int main() {
    std::cout << "=== Testing Complex Patterns ===" << std::endl;
    
    test_pattern("Email-like", "[a-z]+@[a-z]+\\.[a-z]+", "contact user@example.com for info");
    test_pattern("IP-like", "\\d+\\.\\d+\\.\\d+\\.\\d+", "Server IP is 192.168.1.100");
    test_pattern("Nested groups", "(a(b(c)+)+)+", "abcbcabcbcbc");
    
    std::string long_text(10000, 'x');
    long_text += "needle";
    test_pattern("Long input search", "needle", long_text);
    
    return 0;
}
