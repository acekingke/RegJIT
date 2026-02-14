/**
 * Python re Compatibility Test Suite for RegJIT (C++)
 * 
 * This test verifies that RegJIT's behavior matches Python's re module
 * for various regex patterns and test cases.
 * 
 * Build: make test_python_re_compat_cpp
 * Run: ./test_python_re_compat_cpp
 */

#include "../src/regjit.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>

struct TestCase {
    std::string pattern;
    std::string input;
    int expected;  // 1 = should match, 0 = should not match, -1 = compile should fail
    std::string description;
};

int test_count = 0;
int pass_count = 0;
int fail_count = 0;

void run_test(const TestCase& tc) {
    test_count++;
    bool compile_ok = false;
    int result = -2;  // -2 means not executed
    
    try {
        Initialize();
        compile_ok = CompileRegex(tc.pattern);
        
        if (compile_ok && tc.expected != -1) {
            result = Execute(tc.input.c_str());
        }
        CleanUp();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        compile_ok = false;
    }
    
    bool passed = false;
    
    if (tc.expected == -1) {
        // Expect compile failure
        passed = !compile_ok;
    } else {
        passed = compile_ok && (result == tc.expected);
    }
    
    if (passed) {
        pass_count++;
    } else {
        fail_count++;
        std::cout << "FAIL: " << tc.description << std::endl;
        std::cout << "      Pattern: '" << tc.pattern << "'" << std::endl;
        std::cout << "      Input: '" << tc.input << "'" << std::endl;
        std::cout << "      Expected: " << tc.expected << ", Got: ";
        if (!compile_ok) {
            std::cout << "compile_failed";
        } else {
            std::cout << result;
        }
        std::cout << std::endl;
    }
}

void test_basic_literals() {
    std::cout << "\n=== Basic Literal Tests ===" << std::endl;
    std::vector<TestCase> tests = {
        {"a", "a", 1, "Single char match"},
        {"a", "b", 0, "Single char no match"},
        {"a", "abc", 1, "Single char in string"},
        {"abc", "abc", 1, "Literal string exact"},
        {"abc", "xabc", 1, "Literal string search"},
        {"abc", "ab", 0, "Literal string partial"},
    };
    for (const auto& tc : tests) run_test(tc);
}

void test_quantifiers() {
    std::cout << "\n=== Quantifier Tests ===" << std::endl;
    std::vector<TestCase> tests = {
        // Star (*)
        {"a*", "", 1, "Star matches empty"},
        {"a*", "a", 1, "Star matches one"},
        {"a*", "aaa", 1, "Star matches many"},
        {"ab*c", "ac", 1, "Star zero times"},
        {"ab*c", "abc", 1, "Star one time"},
        {"ab*c", "abbbc", 1, "Star many times"},
        
        // Plus (+)
        {"a+", "", 0, "Plus requires one"},
        {"a+", "a", 1, "Plus matches one"},
        {"a+", "aaa", 1, "Plus matches many"},
        {"ab+c", "ac", 0, "Plus requires at least one"},
        {"ab+c", "abc", 1, "Plus one time"},
        {"ab+c", "abbbc", 1, "Plus many times"},
        
        // Question mark (?)
        {"a?", "", 1, "Optional matches empty"},
        {"a?", "a", 1, "Optional matches one"},
        {"ab?c", "ac", 1, "Optional zero"},
        {"ab?c", "abc", 1, "Optional one"},
        {"ab?c", "abbc", 0, "Optional max one"},
        
        // Exact {n}
        {"a{3}", "aaa", 1, "Exact three"},
        {"a{3}", "aa", 0, "Exact three - too few"},
        {"a{3}", "aaaa", 1, "Exact three - search mode finds"},
        
        // At least {n,}
        {"a{2,}", "a", 0, "At least 2 - one fails"},
        {"a{2,}", "aa", 1, "At least 2 - exact"},
        {"a{2,}", "aaaaa", 1, "At least 2 - many"},
        
        // Range {n,m}
        {"a{2,4}", "a", 0, "Range - too few"},
        {"a{2,4}", "aa", 1, "Range - min"},
        {"a{2,4}", "aaa", 1, "Range - middle"},
        {"a{2,4}", "aaaa", 1, "Range - max"},
        {"a{2,4}", "aaaaa", 1, "Range - search finds subset"},
    };
    for (const auto& tc : tests) run_test(tc);
}

void test_anchors() {
    std::cout << "\n=== Anchor Tests ===" << std::endl;
    std::vector<TestCase> tests = {
        // Start anchor (^)
        {"^abc", "abc", 1, "Start anchor match"},
        {"^abc", "xabc", 0, "Start anchor no match"},
        {"^", "anything", 1, "Start anchor alone"},
        {"^", "", 1, "Start anchor empty string"},
        
        // End anchor ($)
        {"abc$", "abc", 1, "End anchor match"},
        {"abc$", "abcx", 0, "End anchor no match"},
        {"abc$", "xabc", 1, "End anchor with prefix"},
        {"$", "anything", 1, "End anchor alone"},
        {"$", "", 1, "End anchor empty string"},
        
        // Both anchors
        {"^abc$", "abc", 1, "Both anchors exact"},
        {"^abc$", "abcd", 0, "Both anchors extra suffix"},
        {"^abc$", "xabc", 0, "Both anchors extra prefix"},
        {"^$", "", 1, "Empty anchored match"},
        {"^$", "a", 0, "Empty anchored no match"},
        
        // Word boundary (\b)
        {"\\bword", "word", 1, "Word boundary start"},
        {"\\bword", " word", 1, "Word boundary after space"},
        {"\\bword", "xword", 0, "No word boundary after x"},
        {"word\\b", "word", 1, "Word boundary end"},
        {"word\\b", "word ", 1, "Word boundary before space"},
        {"word\\b", "wordx", 0, "No word boundary before x"},
        
        // Non-word boundary (\B)
        {"\\Bword", "xword", 1, "Non-word boundary start"},
        {"\\Bword", "word", 0, "Non-word boundary fails at start"},
        {"word\\B", "wordx", 1, "Non-word boundary end"},
        {"word\\B", "word", 0, "Non-word boundary fails at end"},
    };
    for (const auto& tc : tests) run_test(tc);
}

void test_character_classes() {
    std::cout << "\n=== Character Class Tests ===" << std::endl;
    std::vector<TestCase> tests = {
        // Simple class
        {"[abc]", "a", 1, "Class match a"},
        {"[abc]", "b", 1, "Class match b"},
        {"[abc]", "c", 1, "Class match c"},
        {"[abc]", "d", 0, "Class no match d"},
        
        // Range class
        {"[a-z]", "a", 1, "Range start"},
        {"[a-z]", "m", 1, "Range middle"},
        {"[a-z]", "z", 1, "Range end"},
        {"[a-z]", "A", 0, "Range case sensitive"},
        {"[a-z]", "0", 0, "Range no digit"},
        
        // Multiple ranges
        {"[a-zA-Z]", "a", 1, "Multi-range lower"},
        {"[a-zA-Z]", "Z", 1, "Multi-range upper"},
        {"[a-zA-Z]", "0", 0, "Multi-range no digit"},
        
        // Alphanumeric
        {"[a-zA-Z0-9]", "a", 1, "Alnum letter"},
        {"[a-zA-Z0-9]", "5", 1, "Alnum digit"},
        {"[a-zA-Z0-9]", " ", 0, "Alnum no space"},
        
        // Negated class
        {"[^abc]", "d", 1, "Negated match"},
        {"[^abc]", "a", 0, "Negated no match"},
        {"[^a-z]", "A", 1, "Negated range match"},
        {"[^a-z]", "m", 0, "Negated range no match"},
        
        // Dot
        {".", "a", 1, "Dot matches letter"},
        {".", "1", 1, "Dot matches digit"},
        {".", " ", 1, "Dot matches space"},
        {".", "\n", 0, "Dot no match newline"},
        {"a.c", "abc", 1, "Dot in pattern"},
        {"a.c", "aXc", 1, "Dot any char"},
        {"a.c", "ac", 0, "Dot requires char"},
    };
    for (const auto& tc : tests) run_test(tc);
}

void test_escape_sequences() {
    std::cout << "\n=== Escape Sequence Tests ===" << std::endl;
    std::vector<TestCase> tests = {
        // Digit (\d)
        {"\\d", "0", 1, "Digit 0"},
        {"\\d", "5", 1, "Digit 5"},
        {"\\d", "9", 1, "Digit 9"},
        {"\\d", "a", 0, "Digit no letter"},
        {"\\d+", "123", 1, "Digits with plus"},
        {"\\d+", "abc", 0, "Digits no match"},
        
        // Non-digit (\D)
        {"\\D", "a", 1, "Non-digit letter"},
        {"\\D", " ", 1, "Non-digit space"},
        {"\\D", "5", 0, "Non-digit no digit"},
        
        // Word (\w)
        {"\\w", "a", 1, "Word lower"},
        {"\\w", "Z", 1, "Word upper"},
        {"\\w", "5", 1, "Word digit"},
        {"\\w", "_", 1, "Word underscore"},
        {"\\w", " ", 0, "Word no space"},
        {"\\w", "!", 0, "Word no punct"},
        
        // Non-word (\W)
        {"\\W", " ", 1, "Non-word space"},
        {"\\W", "!", 1, "Non-word punct"},
        {"\\W", "a", 0, "Non-word no letter"},
        {"\\W", "_", 0, "Non-word no underscore"},
        
        // Space (\s)
        {"\\s", " ", 1, "Space space"},
        {"\\s", "\t", 1, "Space tab"},
        {"\\s", "\n", 1, "Space newline"},
        {"\\s", "a", 0, "Space no letter"},
        
        // Non-space (\S)
        {"\\S", "a", 1, "Non-space letter"},
        {"\\S", "!", 1, "Non-space punct"},
        {"\\S", " ", 0, "Non-space no space"},
        {"\\S", "\t", 0, "Non-space no tab"},
        
        // Literal escapes
        {"\\t", "\t", 1, "Tab escape"},
        {"\\t", "t", 0, "Tab no literal t"},
        {"\\n", "\n", 1, "Newline escape"},
        {"\\n", "n", 0, "Newline no literal n"},
        {"\\r", "\r", 1, "Carriage return escape"},
        
        // Escaped metacharacters
        {"\\.", ".", 1, "Escaped dot"},
        {"\\.", "a", 0, "Escaped dot no letter"},
        {"\\*", "*", 1, "Escaped star"},
        {"\\+", "+", 1, "Escaped plus"},
        {"\\?", "?", 1, "Escaped question"},
        {"\\\\", "\\", 1, "Escaped backslash"},
        {"\\[", "[", 1, "Escaped bracket"},
        {"\\(", "(", 1, "Escaped paren"},
    };
    for (const auto& tc : tests) run_test(tc);
}

void test_alternation() {
    std::cout << "\n=== Alternation Tests ===" << std::endl;
    std::vector<TestCase> tests = {
        {"a|b", "a", 1, "Alt first"},
        {"a|b", "b", 1, "Alt second"},
        {"a|b", "c", 0, "Alt neither"},
        {"abc|def", "abc", 1, "Alt word first"},
        {"abc|def", "def", 1, "Alt word second"},
        {"abc|def", "ab", 0, "Alt word partial"},
        {"a|b|c", "a", 1, "Multi alt first"},
        {"a|b|c", "b", 1, "Multi alt middle"},
        {"a|b|c", "c", 1, "Multi alt last"},
        {"a|b|c", "d", 0, "Multi alt none"},
        {"(a|b)c", "ac", 1, "Grouped alt first"},
        {"(a|b)c", "bc", 1, "Grouped alt second"},
        {"(a|b)c", "cc", 0, "Grouped alt neither"},
    };
    for (const auto& tc : tests) run_test(tc);
}

void test_groups() {
    std::cout << "\n=== Group Tests ===" << std::endl;
    std::vector<TestCase> tests = {
        {"(abc)", "abc", 1, "Basic group"},
        {"(abc)", "abcd", 1, "Basic group prefix"},
        {"(a)(b)(c)", "abc", 1, "Multiple groups"},
        {"(ab)+", "ab", 1, "Quantified group once"},
        {"(ab)+", "abab", 1, "Quantified group twice"},
        {"(ab)+", "ababab", 1, "Quantified group thrice"},
        {"(ab)+", "a", 0, "Quantified group incomplete"},
        {"(?:abc)", "abc", 1, "Non-capturing group"},
        {"(?:ab)+", "abab", 1, "Non-capturing quantified"},
    };
    for (const auto& tc : tests) run_test(tc);
}

void test_syntax_errors() {
    std::cout << "\n=== Syntax Error Tests ===" << std::endl;
    std::vector<TestCase> tests = {
        // Leading quantifiers
        {"*a", "a", -1, "Leading star"},
        {"+a", "a", -1, "Leading plus"},
        {"?a", "a", -1, "Leading question"},
        {"{2}a", "a", -1, "Leading brace"},
        
        // Double quantifiers (note: a++ is possessive in Python 3.11+ but we reject)
        {"a**", "a", -1, "Double star"},
        {"a++", "a", -1, "Double plus (RegJIT rejects, Python 3.11+ accepts as possessive)"},
        {"a{2}{3}", "a", -1, "Double brace"},
        
        // Anchor quantifiers (Python re: "nothing to repeat")
        {"^*", "a", -1, "Quantified start anchor"},
        {"^+", "a", -1, "Plus on start anchor"},
        {"$*", "a", -1, "Quantified end anchor"},
        {"$+", "a", -1, "Plus on end anchor"},
        {"\\b*", "a", -1, "Quantified word boundary"},
        {"\\b+", "a", -1, "Plus on word boundary"},
        {"\\B*", "a", -1, "Quantified non-word boundary"},
        {"\\B+", "a", -1, "Plus on non-word boundary"},
        
        // Unbalanced parentheses
        {"(", "a", -1, "Unclosed paren"},
        {")", "a", -1, "Unmatched close paren"},
        
        // Empty character class
        {"[]", "a", -1, "Empty char class"},
        {"[^]", "a", -1, "Empty negated class"},
        {"[", "a", -1, "Unclosed char class"},
        
        // Invalid range
        {"[z-a]", "a", -1, "Reversed range"},
        
        // Note: a{2 is treated as literal in Python, but RegJIT may error
        // We test this separately
    };
    for (const auto& tc : tests) run_test(tc);
}

void test_combined_patterns() {
    std::cout << "\n=== Combined Pattern Tests ===" << std::endl;
    std::vector<TestCase> tests = {
        // Email-like patterns
        {"\\w+@\\w+", "user@domain", 1, "Email-like match"},
        {"\\w+@\\w+", "@domain", 0, "Email-like no user"},
        {"^\\w+@\\w+$", "user@domain", 1, "Anchored email"},
        {"^\\w+@\\w+$", " user@domain", 0, "Anchored email spaces"},
        
        // Number patterns
        {"^\\d+$", "123", 1, "Pure digits"},
        {"^\\d+$", "abc", 0, "Pure digits no letters"},
        {"^\\d+$", "12a3", 0, "Pure digits mixed"},
        
        // Word patterns
        {"^\\w+$", "hello", 1, "Pure word"},
        {"^\\w+$", "test_123", 1, "Pure word with underscore"},
        {"^\\w+$", "hello world", 0, "Pure word with space"},
        
        // Complex patterns
        {"^ab*c$", "ac", 1, "Star anchored zero"},
        {"^ab*c$", "abc", 1, "Star anchored one"},
        {"^ab*c$", "abbbc", 1, "Star anchored many"},
        {"^ab*c$", "abbbcd", 0, "Star anchored extra"},
    };
    for (const auto& tc : tests) run_test(tc);
}

void test_python_re_differences() {
    std::cout << "\n=== Python re Behavior Differences ===" << std::endl;
    std::cout << "Note: These test known differences between RegJIT and Python re" << std::endl;
    
    // In Python re, 'a{2' is treated as literal 'a{2', not an error
    // RegJIT treats it as an error
    Initialize();
    bool ok1 = CompileRegex("a{2");
    CleanUp();
    std::cout << "Pattern 'a{2': RegJIT=" << (ok1 ? "compiles" : "error") 
              << " (Python=literal match)" << std::endl;
    
    // In Python 3.11+, 'a++' is possessive quantifier
    // RegJIT treats it as an error
    Initialize();
    bool ok2 = CompileRegex("a++");
    CleanUp();
    std::cout << "Pattern 'a++': RegJIT=" << (ok2 ? "compiles" : "error") 
              << " (Python 3.11+=possessive)" << std::endl;
}

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "RegJIT Python re Compatibility Tests" << std::endl;
    std::cout << "======================================" << std::endl;
    
    test_basic_literals();
    test_quantifiers();
    test_anchors();
    test_character_classes();
    test_escape_sequences();
    test_alternation();
    test_groups();
    test_syntax_errors();
    test_combined_patterns();
    
    std::cout << "\n======================================" << std::endl;
    std::cout << "Test Results: " << pass_count << "/" << test_count << " passed" << std::endl;
    if (fail_count > 0) {
        std::cout << "              " << fail_count << " tests failed" << std::endl;
    }
    std::cout << "======================================" << std::endl;
    
    test_python_re_differences();
    
    return (fail_count == 0) ? 0 : 1;
}
