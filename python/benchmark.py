#!/usr/bin/env python3
"""
RegJIT vs Python re Benchmark

Compares RegJIT (via Python bindings) against Python's standard `re` module.
This benchmark mirrors the C++ benchmark.cpp for Python re compatibility.
"""

import sys
import os
import timeit
import re
from typing import Dict, List, Tuple, Optional

# Add python directory to path for _regjit module
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Benchmark settings (matching C++ benchmark.cpp)
ITERATIONS = 10000
WARMUP = 100


class BenchResult:
    def __init__(self, name: str, pattern: str, input_str: str):
        self.name = name
        self.pattern = pattern
        self.input = input_str[:17] + "..." if len(input_str) > 20 else input_str
        self.jit_ns = 0
        self.re_ns = 0
        self.speedup_vs_re = 0.0


def benchmark_re(
    pattern: str, input_str: str, iterations: int = ITERATIONS, warmup: int = WARMUP
) -> float:
    """Benchmark Python re module."""
    # Compile pattern
    compiled = re.compile(pattern)

    # Warmup
    for _ in range(warmup):
        compiled.search(input_str)

    # Benchmark
    start = timeit.default_timer()
    for _ in range(iterations):
        compiled.search(input_str)
    end = timeit.default_timer()

    # Return nanoseconds per iteration
    return (end - start) * 1_000_000_000 / iterations


def benchmark_jit(
    pattern: str, input_str: str, iterations: int = ITERATIONS, warmup: int = WARMUP
) -> float:
    """Benchmark RegJIT via Python bindings."""
    from _regjit import Regex

    # Compile pattern
    jit_regex = Regex(pattern)

    # Warmup
    for _ in range(warmup):
        jit_regex.match(input_str)

    # Benchmark
    start = timeit.default_timer()
    for _ in range(iterations):
        jit_regex.match(input_str)
    end = timeit.default_timer()

    # Return nanoseconds per iteration
    return (end - start) * 1_000_000_000 / iterations


def run_benchmark(name: str, pattern: str, input_str: str) -> BenchResult:
    """Run a single benchmark case."""
    result = BenchResult(name, pattern, input_str)

    print(f"  Testing: {name} ({pattern})...", end=" ", flush=True)

    # Benchmark RegJIT
    try:
        result.jit_ns = benchmark_jit(pattern, input_str)
    except Exception as e:
        print(f"\n  ERROR: RegJIT failed for pattern '{pattern}': {e}")
        result.jit_ns = 0

    # Benchmark Python re
    try:
        result.re_ns = benchmark_re(pattern, input_str)
    except Exception as e:
        print(f"\n  ERROR: Python re failed for pattern '{pattern}': {e}")
        result.re_ns = 0

    # Calculate speedup
    if result.jit_ns > 0:
        result.speedup_vs_re = result.re_ns / result.jit_ns

    print("done")

    return result


def print_results(results: List[BenchResult]):
    """Print benchmark results in a table format."""
    print("\n" + "=" * 100)
    print("                        RegJIT vs Python re Benchmark Results")
    print("=" * 100)
    print(
        f"{'Test Case':<22} {'Pattern':<18} {'RegJIT(ns)':<12} {'Python re':<14} {'Speedup':<10}"
    )
    print("-" * 100)

    total_speedup = 0.0
    count = 0

    for r in results:
        pattern_display = r.pattern[:13] + "..." if len(r.pattern) > 16 else r.pattern
        speedup_display = f"{r.speedup_vs_re:.2f}x" if r.speedup_vs_re > 0 else "N/A"

        print(
            f"{r.name:<22} {pattern_display:<18} {r.jit_ns:<12.0f} {r.re_ns:<14.0f} {speedup_display:<10}"
        )

        if r.speedup_vs_re > 0:
            total_speedup += r.speedup_vs_re
            count += 1

    print("-" * 100)

    avg_speedup = total_speedup / count if count > 0 else 0
    print(f"{'Average Speedup:':<42} {'':<12} {'':<14} {avg_speedup:.2f}x")
    print("=" * 100)


def main():
    print("Running RegJIT vs Python re benchmarks...")
    print(f"Iterations per test: {ITERATIONS}")
    print(f"Warmup iterations: {WARMUP}")

    results: List[BenchResult] = []

    # ========== Basic Character Matching ==========
    print("\n[1/8] Basic character matching...")
    results.append(run_benchmark("Simple literal", "hello", "hello world"))
    results.append(
        run_benchmark("Long literal", "abcdefghij", "xxxxxxxxxxabcdefghijyyyyyyyyyy")
    )

    # ========== Quantifiers ==========
    print("\n[2/8] Quantifier benchmarks...")
    long_a = "a" * 1000
    results.append(run_benchmark("Exact repeat {1000}", "a{1000}", long_a))
    results.append(run_benchmark("Plus quantifier a+", "a+", long_a))
    results.append(run_benchmark("Star quantifier a*", "a*", long_a))

    # ========== Character Classes ==========
    print("\n[3/8] Character class benchmarks...")
    alphanum = "abc123XYZ789def456GHI"
    results.append(run_benchmark("Char class [a-z]+", "[a-z]+", alphanum))
    results.append(run_benchmark("Char class [a-zA-Z0-9]+", "[a-zA-Z0-9]+", alphanum))
    results.append(run_benchmark("Negated class [^0-9]+", "[^0-9]+", alphanum))

    # ========== Escape Sequences ==========
    print("\n[4/8] Escape sequence benchmarks...")
    digits = "1234567890"
    words = "hello_world_123"
    mixed = "  \t\n  text  \r\n  "
    results.append(run_benchmark("Digit \\d+", r"\d+", digits))
    results.append(run_benchmark("Word \\w+", r"\w+", words))
    results.append(run_benchmark("Whitespace \\s+", r"\s+", mixed))

    # ========== Alternation ==========
    print("\n[5/8] Alternation benchmarks...")
    results.append(run_benchmark("Simple alternation", "cat|dog|bird", "I have a dog"))
    results.append(
        run_benchmark(
            "Complex alternation", "hello|world|foo|bar|baz", "the world is beautiful"
        )
    )

    # ========== Anchors ==========
    print("\n[6/8] Anchor benchmarks...")
    results.append(run_benchmark("Start anchor ^hello", "^hello", "hello world"))
    results.append(run_benchmark("End anchor world$", "world$", "hello world"))
    results.append(run_benchmark("Both anchors ^...$", "^hello world$", "hello world"))

    # ========== Complex Patterns ==========
    print("\n[7/8] Complex pattern benchmarks...")
    results.append(
        run_benchmark(
            "Email-like pattern",
            r"[a-z]+@[a-z]+\.[a-z]+",
            "contact user@example.com for info",
        )
    )
    results.append(
        run_benchmark(
            "IP-like pattern", r"\d+\.\d+\.\d+\.\d+", "Server IP is 192.168.1.100"
        )
    )
    results.append(run_benchmark("Nested groups", r"(a(b(c)+)+)+", "abcbcabcbcbc"))

    # Long input search
    print("\n[8/8] Long input search...")
    long_text = "x" * 10000 + "needle"
    results.append(run_benchmark("Long input search", "needle", long_text))

    # Print results
    print_results(results)

    return 0


if __name__ == "__main__":
    sys.exit(main())
