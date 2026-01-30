#include "../src/regjit.h"
#include <iostream>
#include <cassert>

// Test \d - digit class [0-9]
void test_digit() {
    std::cout << "Testing \\d (digit class)..." << std::endl;
    Initialize();
    bool ok = CompileRegex("\\d");
    assert(ok && "CompileRegex failed for pattern \\d");
    
    // Should match digits
    assert(Execute("0") == 1 && "0 should match \\d");
    assert(Execute("5") == 1 && "5 should match \\d");
    assert(Execute("9") == 1 && "9 should match \\d");
    
    // Should match strings containing digits
    assert(Execute("abc123") == 1 && "abc123 should match \\d");
    assert(Execute("test5") == 1 && "test5 should match \\d");
    
    // Should not match non-digits only
    assert(Execute("abc") == 0 && "abc should not match \\d");
    assert(Execute("") == 0 && "empty should not match \\d");
    
    CleanUp();
    std::cout << "  test_digit passed" << std::endl;
}

// Test \D - non-digit class [^0-9]
void test_non_digit() {
    std::cout << "Testing \\D (non-digit class)..." << std::endl;
    Initialize();
    bool ok = CompileRegex("\\D");
    assert(ok && "CompileRegex failed for pattern \\D");
    
    // Should match non-digits
    assert(Execute("a") == 1 && "a should match \\D");
    assert(Execute("Z") == 1 && "Z should match \\D");
    assert(Execute(" ") == 1 && "space should match \\D");
    
    // Should match strings containing non-digits
    assert(Execute("123abc") == 1 && "123abc should match \\D");
    
    // Should not match digits only
    assert(Execute("123") == 0 && "123 should not match \\D");
    assert(Execute("") == 0 && "empty should not match \\D");
    
    CleanUp();
    std::cout << "  test_non_digit passed" << std::endl;
}

// Test \w - word class [a-zA-Z0-9_]
void test_word() {
    std::cout << "Testing \\w (word class)..." << std::endl;
    Initialize();
    bool ok = CompileRegex("\\w");
    assert(ok && "CompileRegex failed for pattern \\w");
    
    // Should match word characters
    assert(Execute("a") == 1 && "a should match \\w");
    assert(Execute("Z") == 1 && "Z should match \\w");
    assert(Execute("5") == 1 && "5 should match \\w");
    assert(Execute("_") == 1 && "_ should match \\w");
    
    // Should not match non-word characters only
    assert(Execute(" ") == 0 && "space should not match \\w");
    assert(Execute("!") == 0 && "! should not match \\w");
    assert(Execute("") == 0 && "empty should not match \\w");
    
    CleanUp();
    std::cout << "  test_word passed" << std::endl;
}

// Test \W - non-word class [^a-zA-Z0-9_]
void test_non_word() {
    std::cout << "Testing \\W (non-word class)..." << std::endl;
    Initialize();
    bool ok = CompileRegex("\\W");
    assert(ok && "CompileRegex failed for pattern \\W");
    
    // Should match non-word characters
    assert(Execute(" ") == 1 && "space should match \\W");
    assert(Execute("!") == 1 && "! should match \\W");
    assert(Execute("-") == 1 && "- should match \\W");
    
    // Should not match word characters only
    assert(Execute("abc") == 0 && "abc should not match \\W");
    assert(Execute("123") == 0 && "123 should not match \\W");
    assert(Execute("_") == 0 && "_ should not match \\W");
    assert(Execute("") == 0 && "empty should not match \\W");
    
    CleanUp();
    std::cout << "  test_non_word passed" << std::endl;
}

// Test \s - whitespace class [ \t\n\r\f\v]
void test_space() {
    std::cout << "Testing \\s (whitespace class)..." << std::endl;
    Initialize();
    bool ok = CompileRegex("\\s");
    assert(ok && "CompileRegex failed for pattern \\s");
    
    // Should match whitespace
    assert(Execute(" ") == 1 && "space should match \\s");
    assert(Execute("\t") == 1 && "tab should match \\s");
    assert(Execute("\n") == 1 && "newline should match \\s");
    assert(Execute("\r") == 1 && "carriage return should match \\s");
    
    // Should match strings containing whitespace
    assert(Execute("hello world") == 1 && "hello world should match \\s");
    
    // Should not match non-whitespace only
    assert(Execute("abc") == 0 && "abc should not match \\s");
    assert(Execute("") == 0 && "empty should not match \\s");
    
    CleanUp();
    std::cout << "  test_space passed" << std::endl;
}

// Test \S - non-whitespace class [^ \t\n\r\f\v]
void test_non_space() {
    std::cout << "Testing \\S (non-whitespace class)..." << std::endl;
    Initialize();
    bool ok = CompileRegex("\\S");
    assert(ok && "CompileRegex failed for pattern \\S");
    
    // Should match non-whitespace
    assert(Execute("a") == 1 && "a should match \\S");
    assert(Execute("5") == 1 && "5 should match \\S");
    assert(Execute("!") == 1 && "! should match \\S");
    
    // Should not match whitespace only
    assert(Execute(" ") == 0 && "space should not match \\S");
    assert(Execute("\t") == 0 && "tab should not match \\S");
    assert(Execute("\n") == 0 && "newline should not match \\S");
    assert(Execute("") == 0 && "empty should not match \\S");
    
    CleanUp();
    std::cout << "  test_non_space passed" << std::endl;
}

// Test literal escapes \t, \n, \r
void test_literal_escapes() {
    std::cout << "Testing literal escapes (\\t, \\n, \\r)..." << std::endl;
    
    // Test \t (tab)
    Initialize();
    bool ok = CompileRegex("\\t");
    assert(ok && "CompileRegex failed for pattern \\t");
    assert(Execute("\t") == 1 && "tab should match \\t");
    assert(Execute("t") == 0 && "t should not match \\t");
    assert(Execute(" ") == 0 && "space should not match \\t");
    CleanUp();
    
    // Test \n (newline)
    Initialize();
    ok = CompileRegex("\\n");
    assert(ok && "CompileRegex failed for pattern \\n");
    assert(Execute("\n") == 1 && "newline should match \\n");
    assert(Execute("n") == 0 && "n should not match \\n");
    CleanUp();
    
    // Test \r (carriage return)
    Initialize();
    ok = CompileRegex("\\r");
    assert(ok && "CompileRegex failed for pattern \\r");
    assert(Execute("\r") == 1 && "carriage return should match \\r");
    assert(Execute("r") == 0 && "r should not match \\r");
    CleanUp();
    
    std::cout << "  test_literal_escapes passed" << std::endl;
}

// Test combined patterns
void test_combined() {
    std::cout << "Testing combined patterns..." << std::endl;
    
    // Test \d+ (one or more digits)
    Initialize();
    bool ok = CompileRegex("^\\d+$");
    assert(ok && "CompileRegex failed for pattern ^\\d+$");
    assert(Execute("123") == 1 && "123 should match ^\\d+$");
    assert(Execute("0") == 1 && "0 should match ^\\d+$");
    assert(Execute("abc") == 0 && "abc should not match ^\\d+$");
    assert(Execute("12a") == 0 && "12a should not match ^\\d+$");
    CleanUp();
    
    // Test \w+ (one or more word chars)
    Initialize();
    ok = CompileRegex("^\\w+$");
    assert(ok && "CompileRegex failed for pattern ^\\w+$");
    assert(Execute("hello") == 1 && "hello should match ^\\w+$");
    assert(Execute("test_123") == 1 && "test_123 should match ^\\w+$");
    assert(Execute("hello world") == 0 && "hello world should not match ^\\w+$");
    CleanUp();
    
    // Test email-like pattern: \w+@\w+
    Initialize();
    ok = CompileRegex("\\w+@\\w+");
    assert(ok && "CompileRegex failed for pattern \\w+@\\w+");
    assert(Execute("user@domain") == 1 && "user@domain should match \\w+@\\w+");
    assert(Execute("test123@example") == 1 && "test123@example should match");
    assert(Execute("@domain") == 0 && "@domain should not match");
    CleanUp();
    
    std::cout << "  test_combined passed" << std::endl;
}

int main() {
    std::cout << "=== Escape Sequence Tests ===" << std::endl;
    
    test_digit();
    test_non_digit();
    test_word();
    test_non_word();
    test_space();
    test_non_space();
    test_literal_escapes();
    test_combined();
    
    std::cout << "\n[All escape sequence tests passed]" << std::endl;
    return 0;
}
