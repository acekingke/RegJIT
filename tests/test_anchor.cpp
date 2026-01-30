#include "../src/regjit.h"
#include <iostream>
#include <string>

int main() {
    std::cout << "Testing anchor functionality...\n";
    
    // Test 1: ^ anchor (line start)
    std::cout << "\nTesting ^ anchor:\n";
    Initialize();
    bool ok = CompileRegex(std::string("^abc"));
    assert(ok && "CompileRegex failed for pattern ^abc");
    std::cout << "'abc' -> "; Execute("abc");  // Should succeed
    CleanUp();
    
    Initialize();
    ok = CompileRegex(std::string("^abc"));
    assert(ok && "CompileRegex failed for pattern ^abc");
    std::cout << "'xabc' -> "; Execute("xabc"); // Should fail
    CleanUp();
    
    Initialize();
    ok = CompileRegex(std::string("^"));
    assert(ok && "CompileRegex failed for pattern ^");
    std::cout << "'anything' -> "; Execute("anything"); // Should succeed (matches at start)
    CleanUp();
    
    // Test 2: $ anchor (line end)
    std::cout << "\nTesting $ anchor:\n";
    Initialize();
    CompileRegex(std::string("abc$"));
    std::cout << "'abc' -> "; Execute("abc");  // Should succeed
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("abc$"));
    std::cout << "'abcx' -> "; Execute("abcx"); // Should fail
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("abc$"));
    std::cout << "'xabc' -> "; Execute("xabc"); // Should fail
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("$"));
    std::cout << "'anything' -> "; Execute("anything"); // Should succeed (matches at end)
    CleanUp();
    
    // Test 3: ^ and $ together
    std::cout << "\nTesting ^ and $ together:\n";
    Initialize();
    CompileRegex(std::string("^abc$"));
    std::cout << "'abc' -> "; Execute("abc");    // Should succeed
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("^abc$"));
    std::cout << "'xabc' -> "; Execute("xabc");   // Should fail
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("^abc$"));
    std::cout << "'abcx' -> "; Execute("abcx");   // Should fail
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("^abc$"));
    std::cout << "'xabcx' -> "; Execute("xabcx"); // Should fail
    CleanUp();
    
    // Test 4: \b word boundary
    std::cout << "\nTesting \\b word boundary:\n";
    Initialize();
    CompileRegex(std::string("\\babc"));
    std::cout << "'abc' -> "; Execute("abc");     // Should succeed (start of string)
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("\\babc"));
    std::cout << "' abc' -> "; Execute(" abc");   // Should succeed (after space)
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("\\babc"));
    std::cout << "'xabc' -> "; Execute("xabc");   // Should fail (after 'x')
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("abc\\b"));
    std::cout << "'abc' -> "; Execute("abc");     // Should succeed (end of string)
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("abc\\b"));
    std::cout << "'abc ' -> "; Execute("abc ");   // Should succeed (before space)
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("abc\\b"));
    std::cout << "'abcx' -> "; Execute("abcx");   // Should fail (before 'x')
    CleanUp();
    
    // Test 5: \B non-word boundary
    std::cout << "\nTesting \\B non-word boundary:\n";
    Initialize();
    CompileRegex(std::string("\\Babc"));
    std::cout << "'abc' -> "; Execute("abc");     // Should fail (start of string)
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("\\Babc"));
    std::cout << "'xabc' -> "; Execute("xabc");   // Should succeed (after 'x')
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("abc\\B"));
    std::cout << "'abc' -> "; Execute("abc");     // Should fail (end of string)
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("abc\\B"));
    std::cout << "'abcx' -> "; Execute("abcx");   // Should succeed (before 'x')
    CleanUp();
    
    // Test 6: Combined with other features
    std::cout << "\nTesting anchors with other features:\n";
    Initialize();
    CompileRegex(std::string("^a.c$"));
    std::cout << "'abc' -> "; Execute("abc");     // Should succeed
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("^a.c$"));
    std::cout << "'axc' -> "; Execute("axc");     // Should succeed
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("^a.c$"));
    std::cout << "'axcx' -> "; Execute("axcx");   // Should fail
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("^ab*c$"));
    std::cout << "'ac' -> "; Execute("ac");       // Should succeed
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("^ab*c$"));
    std::cout << "'abc' -> "; Execute("abc");     // Should succeed
    CleanUp();
    
    Initialize();
    CompileRegex(std::string("^ab*c$"));
    std::cout << "'abbbbc' -> "; Execute("abbbbc"); // Should succeed
    CleanUp();
    
    std::cout << "\nAnchor testing completed.\n";
    return 0;
}
