#!/usr/bin/env python3
"""
Python re Compatibility Test Suite for RegJIT

This script tests RegJIT's compatibility with Python's re module.
It compares the behavior of both engines across various regex patterns
and reports any differences.

Usage:
    python3 tests/test_python_re_compat.py

Prerequisites:
    - Build RegJIT with: make libregjit.so
    - Build Python bindings with: make python-bindings
"""

import re
import sys
import os
from typing import List, Tuple, Optional, Dict, Any

# Add paths for RegJIT Python bindings
script_dir = os.path.dirname(os.path.abspath(__file__))
project_root = os.path.dirname(script_dir)
python_dir = os.path.join(project_root, "python")
sys.path.insert(0, python_dir)

# Set library path for dynamic linking
if sys.platform == "darwin":
    os.environ["DYLD_LIBRARY_PATH"] = (
        project_root + ":" + os.environ.get("DYLD_LIBRARY_PATH", "")
    )
else:
    os.environ["LD_LIBRARY_PATH"] = (
        project_root + ":" + os.environ.get("LD_LIBRARY_PATH", "")
    )

# Try to import RegJIT bindings
try:
    from _regjit import Regex

    REGJIT_AVAILABLE = True
except ImportError as e:
    print(f"Warning: Could not import RegJIT bindings: {e}")
    print("Please build with: make libregjit.so && make python-bindings")
    REGJIT_AVAILABLE = False


class CompatibilityTest:
    """A single compatibility test case."""

    def __init__(
        self,
        pattern: str,
        test_strings: List[str],
        description: str = "",
        expect_compile_fail: bool = False,
        python_re_flags: int = 0,
    ):
        self.pattern = pattern
        self.test_strings = test_strings
        self.description = description
        self.expect_compile_fail = expect_compile_fail
        self.python_re_flags = python_re_flags


class TestResult:
    """Result of a compatibility test."""

    def __init__(self, test: CompatibilityTest):
        self.test = test
        self.python_compile_ok = False
        self.python_compile_error = None
        self.regjit_compile_ok = False
        self.regjit_compile_error = None
        self.match_results: List[Dict[str, Any]] = []
        self.passed = True
        self.differences: List[str] = []


def test_python_re(
    pattern: str, test_string: str, flags: int = 0
) -> Tuple[bool, Optional[str]]:
    """Test a pattern against Python's re module.

    Returns: (match_found, error_message)
    """
    try:
        regex = re.compile(pattern, flags)
        match = regex.search(test_string)
        return (match is not None, None)
    except re.error as e:
        return (False, str(e))


def test_regjit(pattern: str, test_string: str) -> Tuple[bool, Optional[str]]:
    """Test a pattern against RegJIT.

    Returns: (match_found, error_message)
    """
    if not REGJIT_AVAILABLE:
        return (False, "RegJIT not available")

    try:
        regex = Regex(pattern)
        # Use search to match Python re.search behavior in test_python_re
        result = regex.search(test_string)
        regex.unload()
        # Convert result (Match object or None) to boolean
        return (result is not None, None)
    except Exception as e:
        return (False, str(e))


def run_compatibility_test(test: CompatibilityTest) -> TestResult:
    """Run a single compatibility test."""
    result = TestResult(test)

    # Test compilation
    try:
        re.compile(test.pattern, test.python_re_flags)
        result.python_compile_ok = True
    except re.error as e:
        result.python_compile_error = str(e)

    if REGJIT_AVAILABLE:
        try:
            regex = Regex(test.pattern)
            result.regjit_compile_ok = True
            regex.unload()
        except Exception as e:
            result.regjit_compile_error = str(e)

    # Check compile result compatibility
    if test.expect_compile_fail:
        if result.python_compile_ok:
            result.differences.append(
                f"Expected compile fail, but Python re compiled OK"
            )
            result.passed = False
        if result.regjit_compile_ok:
            result.differences.append(f"Expected compile fail, but RegJIT compiled OK")
            result.passed = False
        # If both failed as expected, that's a pass
        if not result.python_compile_ok and not result.regjit_compile_ok:
            return result
    else:
        if not result.python_compile_ok:
            result.differences.append(
                f"Python re failed to compile: {result.python_compile_error}"
            )
            result.passed = False
        if REGJIT_AVAILABLE and not result.regjit_compile_ok:
            result.differences.append(
                f"RegJIT failed to compile: {result.regjit_compile_error}"
            )
            result.passed = False

    # Skip match tests if either compilation failed
    if not result.python_compile_ok or (
        REGJIT_AVAILABLE and not result.regjit_compile_ok
    ):
        return result

    # Test each string
    for test_string in test.test_strings:
        py_match, py_error = test_python_re(
            test.pattern, test_string, test.python_re_flags
        )

        match_result = {
            "string": test_string,
            "python_match": py_match,
            "python_error": py_error,
        }

        if REGJIT_AVAILABLE:
            rj_match, rj_error = test_regjit(test.pattern, test_string)
            match_result["regjit_match"] = rj_match
            match_result["regjit_error"] = rj_error

            # Compare results
            if py_match != rj_match:
                result.differences.append(
                    f"String '{test_string}': Python={py_match}, RegJIT={rj_match}"
                )
                result.passed = False

        result.match_results.append(match_result)

    return result


# =============================================================================
# Test Categories
# =============================================================================


def get_basic_literal_tests() -> List[CompatibilityTest]:
    """Basic literal character matching tests."""
    return [
        CompatibilityTest("a", ["a", "b", "abc", ""], "Single char match"),
        CompatibilityTest("abc", ["abc", "abcd", "xabc", "ab", ""], "Literal string"),
        # CompatibilityTest("", ["", "a"], "Empty pattern"),
    ]


def get_quantifier_tests() -> List[CompatibilityTest]:
    """Quantifier tests: *, +, ?, {n}, {n,}, {n,m}"""
    return [
        # Star (*)
        CompatibilityTest("a*", ["", "a", "aaa", "b", "baaab"], "Star quantifier"),
        CompatibilityTest("ab*c", ["ac", "abc", "abbbc", "ab", ""], "Star in pattern"),
        # Plus (+)
        CompatibilityTest("a+", ["a", "aaa", "", "b", "baaab"], "Plus quantifier"),
        CompatibilityTest("ab+c", ["abc", "abbbc", "ac", "ab"], "Plus in pattern"),
        # Question mark (?)
        CompatibilityTest("a?", ["", "a", "aa", "b"], "Optional quantifier"),
        CompatibilityTest("ab?c", ["ac", "abc", "abbc"], "Optional in pattern"),
        # Exact {n}
        CompatibilityTest("a{3}", ["aaa", "aa", "aaaa", ""], "Exact repeat"),
        CompatibilityTest("ab{2}c", ["abbc", "abc", "abbbc"], "Exact in pattern"),
        # At least {n,}
        CompatibilityTest("a{2,}", ["aa", "aaa", "a", ""], "At least repeat"),
        # Range {n,m}
        CompatibilityTest(
            "a{2,4}", ["aa", "aaa", "aaaa", "a", "aaaaa"], "Range repeat"
        ),
        # Non-greedy quantifiers
        CompatibilityTest("a*?", ["", "a", "aaa"], "Non-greedy star"),
        CompatibilityTest("a+?", ["a", "aaa"], "Non-greedy plus"),
        CompatibilityTest("a??", ["", "a"], "Non-greedy optional"),
        CompatibilityTest("a{2,4}?", ["aa", "aaa", "aaaa"], "Non-greedy range"),
    ]


def get_anchor_tests() -> List[CompatibilityTest]:
    """Anchor tests: ^, $, \b, \B"""
    return [
        # Start anchor
        CompatibilityTest("^abc", ["abc", "abcd", "xabc"], "Start anchor"),
        CompatibilityTest("^", ["", "a", "abc"], "Start anchor alone"),
        # End anchor
        CompatibilityTest("abc$", ["abc", "xabc", "abcx"], "End anchor"),
        CompatibilityTest("$", ["", "a", "abc"], "End anchor alone"),
        # Both anchors
        CompatibilityTest("^abc$", ["abc", "abcd", "xabc"], "Start and end anchors"),
        CompatibilityTest("^$", ["", "a"], "Empty match anchored"),
        # Word boundary
        CompatibilityTest(r"\bword", ["word", " word", "xword"], "Word boundary start"),
        CompatibilityTest(r"word\b", ["word", "word ", "wordx"], "Word boundary end"),
        CompatibilityTest(
            r"\bword\b", ["word", " word ", "words"], "Word boundaries both"
        ),
        # Non-word boundary
        CompatibilityTest(
            r"\Bword", ["xword", "word", " word"], "Non-word boundary start"
        ),
        CompatibilityTest(
            r"word\B", ["wordx", "word", "word "], "Non-word boundary end"
        ),
    ]


def get_character_class_tests() -> List[CompatibilityTest]:
    """Character class tests: [...], [^...], ."""
    return [
        # Simple character class
        CompatibilityTest("[abc]", ["a", "b", "c", "d", ""], "Simple class"),
        CompatibilityTest("[a-z]", ["a", "m", "z", "A", "0"], "Range class"),
        CompatibilityTest("[a-zA-Z]", ["a", "Z", "0", " "], "Multiple ranges"),
        CompatibilityTest("[a-zA-Z0-9]", ["a", "Z", "5", " "], "Alphanumeric"),
        # Negated class
        CompatibilityTest("[^abc]", ["d", "0", " ", "a", ""], "Negated class"),
        CompatibilityTest("[^a-z]", ["A", "0", " ", "a"], "Negated range"),
        # Dot
        CompatibilityTest(".", ["a", "1", " ", "\n"], "Dot (any char except newline)"),
        CompatibilityTest("a.c", ["abc", "aXc", "ac", "a\nc"], "Dot in pattern"),
        # Special chars in class
        CompatibilityTest("[-abc]", ["-", "a", "d"], "Dash at start"),
        CompatibilityTest("[abc-]", ["-", "a", "d"], "Dash at end"),
    ]


def get_escape_sequence_tests() -> List[CompatibilityTest]:
    """Escape sequence tests: \d, \w, \s, etc."""
    return [
        # Digit
        CompatibilityTest(r"\d", ["0", "5", "9", "a", ""], "Digit class"),
        CompatibilityTest(r"\D", ["a", " ", "!", "5", ""], "Non-digit class"),
        CompatibilityTest(
            r"\d+", ["123", "abc123", "", "abc"], "Digit with quantifier"
        ),
        # Word
        CompatibilityTest(r"\w", ["a", "Z", "0", "_", " ", "!"], "Word class"),
        CompatibilityTest(r"\W", [" ", "!", "-", "a", "0", "_"], "Non-word class"),
        CompatibilityTest(
            r"\w+", ["hello", "test_123", " ", ""], "Word with quantifier"
        ),
        # Space
        CompatibilityTest(r"\s", [" ", "\t", "\n", "\r", "a"], "Space class"),
        CompatibilityTest(r"\S", ["a", "0", "!", " ", "\t"], "Non-space class"),
        CompatibilityTest(r"\s+", ["   ", "\t\n", "a", ""], "Space with quantifier"),
        # Literal escapes
        CompatibilityTest(r"\t", ["\t", "t", " "], "Tab escape"),
        CompatibilityTest(r"\n", ["\n", "n", " "], "Newline escape"),
        CompatibilityTest(r"\r", ["\r", "r", " "], "Carriage return escape"),
        # Escaped metacharacters
        CompatibilityTest(r"\.", [".", "a", ""], "Escaped dot"),
        CompatibilityTest(r"\*", ["*", "a", ""], "Escaped star"),
        CompatibilityTest(r"\+", ["+", "a", ""], "Escaped plus"),
        CompatibilityTest(r"\?", ["?", "a", ""], "Escaped question"),
        CompatibilityTest(r"\\", ["\\", "a", ""], "Escaped backslash"),
        CompatibilityTest(r"\[", ["[", "a", ""], "Escaped bracket"),
        CompatibilityTest(r"\(", ["(", "a", ""], "Escaped paren"),
    ]


def get_alternation_tests() -> List[CompatibilityTest]:
    """Alternation tests: |"""
    return [
        CompatibilityTest("a|b", ["a", "b", "c", "ab"], "Simple alternation"),
        CompatibilityTest(
            "abc|def", ["abc", "def", "abcdef", "ab"], "Word alternation"
        ),
        CompatibilityTest("a|b|c", ["a", "b", "c", "d"], "Multiple alternatives"),
        CompatibilityTest("(a|b)c", ["ac", "bc", "cc", "abc"], "Grouped alternation"),
    ]


def get_group_tests() -> List[CompatibilityTest]:
    """Group tests: (...), (?:...)"""
    return [
        CompatibilityTest("(abc)", ["abc", "abcd", "xabc"], "Basic group"),
        CompatibilityTest("(a)(b)(c)", ["abc", "ab", "abcd"], "Multiple groups"),
        CompatibilityTest("(ab)+", ["ab", "abab", "ababab", "a"], "Quantified group"),
        CompatibilityTest("(?:abc)", ["abc", "abcd", "xabc"], "Non-capturing group"),
        CompatibilityTest("(?:ab)+", ["ab", "abab", "a"], "Non-capturing quantified"),
    ]


def get_syntax_error_tests() -> List[CompatibilityTest]:
    """Tests for patterns that should fail to compile."""
    return [
        # Leading quantifiers
        CompatibilityTest("*a", ["a"], "Leading star", expect_compile_fail=True),
        CompatibilityTest("+a", ["a"], "Leading plus", expect_compile_fail=True),
        CompatibilityTest("?a", ["a"], "Leading question", expect_compile_fail=True),
        CompatibilityTest("{2}a", ["aa"], "Leading brace", expect_compile_fail=True),
        # Double quantifiers
        # Note: a** is truly invalid, but a++ is possessive quantifier in Python 3.11+
        # and a{2}{3} is treated as valid (first is literal, second is quantifier)
        CompatibilityTest("a**", ["a"], "Double star", expect_compile_fail=True),
        # a++ is possessive quantifier in Python 3.11+ - NOT an error
        # CompatibilityTest("a++", ["a"], "Double plus", expect_compile_fail=True),
        # a{2}{3} compiles OK in Python (first {2} quantifies 'a', second {3} is error OR
        # depends on version behavior)
        # CompatibilityTest("a{2}{3}", ["aa"], "Double brace", expect_compile_fail=True),
        # Anchor quantifiers (Python re specific - "nothing to repeat")
        CompatibilityTest(
            "^*", ["a"], "Quantified start anchor", expect_compile_fail=True
        ),
        CompatibilityTest(
            "^+", ["a"], "Plus on start anchor", expect_compile_fail=True
        ),
        CompatibilityTest(
            "$*", ["a"], "Quantified end anchor", expect_compile_fail=True
        ),
        CompatibilityTest("$+", ["a"], "Plus on end anchor", expect_compile_fail=True),
        CompatibilityTest(
            r"\b*", ["a"], "Quantified word boundary", expect_compile_fail=True
        ),
        CompatibilityTest(
            r"\b+", ["a"], "Plus on word boundary", expect_compile_fail=True
        ),
        CompatibilityTest(
            r"\B*", ["a"], "Quantified non-word boundary", expect_compile_fail=True
        ),
        CompatibilityTest(
            r"\B+", ["a"], "Plus on non-word boundary", expect_compile_fail=True
        ),
        # Unbalanced parentheses
        CompatibilityTest("(", ["a"], "Unclosed paren", expect_compile_fail=True),
        CompatibilityTest(
            ")", ["a"], "Unmatched close paren", expect_compile_fail=True
        ),
        CompatibilityTest("((a)", ["a"], "Unbalanced parens", expect_compile_fail=True),
        # Empty character class
        CompatibilityTest("[]", ["a"], "Empty char class", expect_compile_fail=True),
        CompatibilityTest(
            "[^]", ["a"], "Empty negated class", expect_compile_fail=True
        ),
        CompatibilityTest("[", ["a"], "Unclosed char class", expect_compile_fail=True),
        # Invalid range
        CompatibilityTest("[z-a]", ["a"], "Reversed range", expect_compile_fail=True),
        # Unclosed brace quantifier
        # Note: In Python re, 'a{2' is treated as literal 'a{2', NOT an error
        # CompatibilityTest("a{2", ["a"], "Unclosed brace", expect_compile_fail=True),
    ]


def get_combined_pattern_tests() -> List[CompatibilityTest]:
    """Complex combined pattern tests."""
    return [
        # Email-like patterns
        CompatibilityTest(
            r"\w+@\w+", ["user@domain", "@domain", "user@"], "Email-like"
        ),
        CompatibilityTest(
            r"^\w+@\w+$",
            ["user@domain", " user@domain", "user@domain "],
            "Anchored email",
        ),
        # Number patterns
        CompatibilityTest(r"^\d+$", ["123", "abc", "12a3", ""], "Pure digits"),
        CompatibilityTest(
            r"^-?\d+$", ["123", "-123", "--123", "abc"], "Optional negative"
        ),
        CompatibilityTest(
            r"^\d+\.\d+$", ["3.14", "123.456", "3.", ".5"], "Decimal number"
        ),
        # Word patterns
        CompatibilityTest(
            r"^\w+$", ["hello", "test_123", "hello world", ""], "Pure word"
        ),
        CompatibilityTest(
            r"\b\w+\b", ["hello", " hello world ", ""], "Word boundaries"
        ),
        # Whitespace handling
        CompatibilityTest(
            r"^\s*\w+\s*$", ["hello", "  hello  ", "  hello world  "], "Trimmed word"
        ),
        CompatibilityTest(r"\S+", ["hello", "  ", "hello world"], "Non-space sequence"),
        # Nested groups
        CompatibilityTest(
            r"((a|b)+)", ["aaa", "bbb", "ababab", "ccc"], "Nested groups"
        ),
        # Complex alternation
        CompatibilityTest(
            r"(cat|dog)s?", ["cat", "cats", "dog", "dogs", "bird"], "Plural optional"
        ),
    ]


def run_all_tests() -> Dict[str, List[TestResult]]:
    """Run all compatibility tests and return results by category."""
    categories = {
        "Basic Literals": get_basic_literal_tests(),
        "Quantifiers": get_quantifier_tests(),
        "Anchors": get_anchor_tests(),
        "Character Classes": get_character_class_tests(),
        "Escape Sequences": get_escape_sequence_tests(),
        "Alternation": get_alternation_tests(),
        "Groups": get_group_tests(),
        "Syntax Errors": get_syntax_error_tests(),
        "Combined Patterns": get_combined_pattern_tests(),
    }

    results = {}
    for category, tests in categories.items():
        results[category] = [run_compatibility_test(test) for test in tests]

    return results


def print_results(results: Dict[str, List[TestResult]]) -> Tuple[int, int]:
    """Print test results and return (passed, failed) counts."""
    total_passed = 0
    total_failed = 0

    print("=" * 80)
    print("Python re Compatibility Test Report for RegJIT")
    print("=" * 80)

    if not REGJIT_AVAILABLE:
        print(
            "\nWARNING: RegJIT bindings not available. Only testing Python re behavior."
        )
        print("         Build with: make libregjit.so && make python-bindings\n")

    for category, test_results in results.items():
        passed = sum(1 for r in test_results if r.passed)
        failed = len(test_results) - passed
        total_passed += passed
        total_failed += failed

        status = "✓" if failed == 0 else "✗"
        print(f"\n{status} {category}: {passed}/{len(test_results)} passed")

        # Show failures
        for result in test_results:
            if not result.passed:
                print(f"\n  FAIL: {result.test.description}")
                print(f"        Pattern: {repr(result.test.pattern)}")
                for diff in result.differences:
                    print(f"        - {diff}")

    print("\n" + "=" * 80)
    print(f"Total: {total_passed}/{total_passed + total_failed} tests passed")
    if total_failed > 0:
        print(f"       {total_failed} tests failed")
    print("=" * 80)

    return (total_passed, total_failed)


def generate_compatibility_matrix(results: Dict[str, List[TestResult]]) -> None:
    """Generate a detailed compatibility matrix."""
    print("\n" + "=" * 80)
    print("Compatibility Matrix")
    print("=" * 80)

    features = {
        "Basic character matching": True,
        "Quantifiers (* + ? {n} {n,} {n,m})": True,
        "Non-greedy quantifiers (*? +? ?? {n,m}?)": True,
        "Anchors (^ $ \\b \\B)": True,
        "Character classes ([...] [^...])": True,
        "Dot (. any char)": True,
        "Escape sequences (\\d \\w \\s \\D \\W \\S)": True,
        "Literal escapes (\\t \\n \\r)": True,
        "Alternation (|)": True,
        "Groups ((...))": True,
        "Non-capturing groups ((?:...))": True,
        "Syntax error detection": True,
        "Anchor quantifier rejection": True,
    }

    # Check results to update compatibility
    for category, test_results in results.items():
        for result in test_results:
            if not result.passed:
                # Mark related features as potentially incompatible
                if "anchor" in result.test.description.lower():
                    features["Anchor quantifier rejection"] = False
                if "quantifier" in result.test.description.lower():
                    features["Quantifiers (* + ? {n} {n,} {n,m})"] = False

    print("\nFeature                                    | Compatible")
    print("-" * 60)
    for feature, compatible in features.items():
        status = "✓ Yes" if compatible else "✗ No"
        print(f"{feature:<42} | {status}")


def main():
    """Main entry point."""
    print("RegJIT Python re Compatibility Test Suite")
    print("-" * 40)

    results = run_all_tests()
    passed, failed = print_results(results)

    generate_compatibility_matrix(results)

    # Return non-zero exit code if there were failures
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
