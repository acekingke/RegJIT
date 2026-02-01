#include "regjit.h"
#include <iostream>
#include <string>
#include <cassert>

// Simple test helper
void test(const std::string& name, const std::string& pattern, 
          const std::string& input, bool expected) {
    Initialize();
    if (!CompileRegex(pattern)) {
        std::cerr << "Failed to compile: " << pattern << std::endl;
        CleanUp();
        return;
    }
    
    int result = Execute(input.c_str());
    bool passed = (result == 1) == expected;
    
    std::cout << (passed ? "PASS" : "FAIL") << ": " << name 
              << " pattern='" << pattern << "' input='" 
              << (input.length() > 30 ? input.substr(0,27) + "..." : input)
              << "' expected=" << expected << " got=" << result << std::endl;
    
    if (!passed) {
        std::cerr << "  ASSERTION FAILED!" << std::endl;
    }
    
    CleanUp();
}

int main() {
    std::cout << "=== Testing memmem optimization ===" << std::endl;
    
    // Basic literal tests
    test("simple match", "hello", "hello world", true);
    test("simple no match", "hello", "goodbye world", false);
    test("literal at end", "needle", "haystackneedle", true);
    test("literal not found", "needle", "haystack", false);
    
    // Long input tests (this is what memmem optimizes)
    std::string longInput(10000, 'x');
    longInput += "needle";
    test("long input with match", "needle", longInput, true);
    
    std::string longNoMatch(10000, 'x');
    test("long input no match", "needle", longNoMatch, false);
    
    // Edge cases
    test("empty input", "abc", "", false);
    test("pattern longer than input", "abcdefgh", "abc", false);
    test("exact match", "abc", "abc", true);
    
    // Multi-char literal
    test("multi-char literal", "abcdef", "xxxabcdefyyy", true);
    test("multi-char no match", "abcdef", "xxxabcdeyyy", false);
    
    std::cout << "\n=== memmem tests completed ===" << std::endl;
    return 0;
}
