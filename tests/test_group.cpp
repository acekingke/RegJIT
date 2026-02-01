#include "../src/regjit.h"
#include <iostream>
#include <cassert>

void test_basic_group() {
    std::cout << "Testing basic groups..." << std::endl;
    Initialize();
    bool ok = CompileRegex("(ab)c");
    assert(ok && "CompileRegex failed for pattern (ab)c");
    assert(Execute("abc") == 1 && "abc should match (ab)c");
    assert(Execute("ab") == 0 && "ab should not match (ab)c");
    assert(Execute("zabc") == 1 && "zabc should match (ab)c (search mode)");
    CleanUp();
    std::cout << "  test_basic_group passed" << std::endl;
}

void test_group_alternation() {
    std::cout << "Testing group alternation..." << std::endl;
    Initialize();
    bool ok = CompileRegex("a(b|c)d");
    assert(ok && "CompileRegex failed for pattern a(b|c)d");
    assert(Execute("abd") == 1 && "abd should match a(b|c)d");
    assert(Execute("acd") == 1 && "acd should match a(b|c)d");
    assert(Execute("ad") == 0 && "ad should not match a(b|c)d");
    assert(Execute("abbd") == 0 && "abbd should not match a(b|c)d");
    CleanUp();
    std::cout << "  test_group_alternation passed" << std::endl;
}

void test_group_quantifier() {
    std::cout << "Testing group quantifier..." << std::endl;
    Initialize();
    bool ok = CompileRegex("^(ab)+$");
    assert(ok && "CompileRegex failed for pattern ^(ab)+$");
    assert(Execute("ab") == 1 && "ab should match ^(ab)+$");
    assert(Execute("abab") == 1 && "abab should match ^(ab)+$");
    assert(Execute("aba") == 0 && "aba should not match ^(ab)+$");
    CleanUp();
    std::cout << "  test_group_quantifier passed" << std::endl;
}

void test_non_capturing_group() {
    std::cout << "Testing non-capturing group..." << std::endl;
    Initialize();
    bool ok = CompileRegex("^(?:ab)+$");
    assert(ok && "CompileRegex failed for pattern ^(?:ab)+$");
    assert(Execute("ab") == 1 && "ab should match ^(?:ab)+$");
    assert(Execute("abab") == 1 && "abab should match ^(?:ab)+$");
    assert(Execute("aba") == 0 && "aba should not match ^(?:ab)+$");
    CleanUp();
    std::cout << "  test_non_capturing_group passed" << std::endl;
}

int main() {
    std::cout << "=== Group Tests ===" << std::endl;
    test_basic_group();
    test_group_alternation();
    test_group_quantifier();
    test_non_capturing_group();
    std::cout << "\n[All group tests passed]" << std::endl;
    return 0;
}
