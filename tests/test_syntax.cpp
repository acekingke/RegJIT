#include "../src/regjit.h"
#include <iostream>
#include <cassert>

void test_leading_quantifiers() {
    std::cout << "Testing leading quantifiers..." << std::endl;
    Initialize();
    assert(!CompileRegex("*a") && "*a should fail to compile (nothing to repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("+a") && "+a should fail to compile (nothing to repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("?a") && "?a should fail to compile (nothing to repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("{2}a") && "{2}a should fail to compile (nothing to repeat)");
    CleanUp();

    std::cout << "  test_leading_quantifiers passed" << std::endl;
}

void test_double_quantifiers() {
    std::cout << "Testing double quantifiers..." << std::endl;
    Initialize();
    assert(!CompileRegex("a**") && "a** should fail to compile (multiple repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("a++") && "a++ should fail to compile (multiple repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("a{2}{3}") && "a{2}{3} should fail to compile (multiple repeat)");
    CleanUp();

    std::cout << "  test_double_quantifiers passed" << std::endl;
}

void test_unbalanced_parentheses() {
    std::cout << "Testing unbalanced parentheses..." << std::endl;
    Initialize();
    assert(!CompileRegex(")") && "')' should fail to compile (unbalanced parenthesis)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("(") && "'(' should fail to compile (Unexpected token)");
    CleanUp();

    std::cout << "  test_unbalanced_parentheses passed" << std::endl;
}

void test_empty_charclass() {
    std::cout << "Testing empty character classes..." << std::endl;
    Initialize();
    assert(!CompileRegex("[]") && "[] should fail to compile (unterminated character set)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("[^]") && "[^] should fail to compile (unterminated character set)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("[") && "'[' should fail to compile (Unclosed character class)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("[a") && "'[a' should fail to compile (Unclosed character class)");
    CleanUp();

    std::cout << "  test_empty_charclass passed" << std::endl;
}

void test_invalid_ranges() {
    std::cout << "Testing invalid character ranges..." << std::endl;
    Initialize();
    assert(!CompileRegex("[z-a]") && "[z-a] should fail to compile (bad character range)");
    CleanUp();

    std::cout << "  test_invalid_ranges passed" << std::endl;
}

void test_unclosed_brace() {
    std::cout << "Testing unclosed braces..." << std::endl;
    Initialize();
    assert(!CompileRegex("a{2") && "a{2 should fail to compile (missing '}')");
    CleanUp();

    std::cout << "  test_unclosed_brace passed" << std::endl;
}

void test_empty_group() {
    std::cout << "Testing empty group..." << std::endl;
    // Python re allows () — it matches the empty string
    Initialize();
    assert(CompileRegex("()") && "() should compile (matches empty string, per Python re)");
    assert(Execute("hello") == 1 && "() should match any input (empty match)");
    assert(Execute("") == 1 && "() should match empty input");
    CleanUp();

    std::cout << "  test_empty_group passed" << std::endl;
}

int main() {
    std::cout << "=== Syntax Error Tests ===" << std::endl;
    test_leading_quantifiers();
    test_double_quantifiers();
    test_unbalanced_parentheses();
    test_empty_charclass();
    test_invalid_ranges();
    test_unclosed_brace();
    test_empty_group();
    std::cout << "\n[All syntax error tests passed]" << std::endl;
    return 0;
}
