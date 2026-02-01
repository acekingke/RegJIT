#include "regjit.h"
#include <iostream>
#include <chrono>
#include <regex>
#include <vector>
#include <string>
#include <iomanip>

// Benchmark result structure
struct BenchResult {
    std::string name;
    std::string pattern;
    std::string input;
    uint64_t jit_ns;
    uint64_t std_ns;
    double speedup;
};

// Number of iterations for each benchmark
constexpr int ITERATIONS = 100000;
constexpr int WARMUP = 1000;

// Benchmark JIT regex
uint64_t benchmark_jit(const std::string& pattern, const std::string& input) {
    Initialize();
    if (!CompileRegex(pattern)) {
        std::cerr << "Failed to compile pattern: " << pattern << std::endl;
        CleanUp();
        return 0;
    }
    
    // Use the FunctionName set by CompileRegex
    std::string lookupName = FunctionName.empty() ? "match" : FunctionName;
    auto MatchSym = ExitOnErr(JIT->lookup(lookupName));
    auto MatchFunc = (int (*)(const char*))MatchSym.getValue();
    
    // Warmup
    for (int i = 0; i < WARMUP; ++i) {
        MatchFunc(input.c_str());
    }
    
    // Benchmark
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        MatchFunc(input.c_str());
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    CleanUp();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / ITERATIONS;
}

// Benchmark std::regex
uint64_t benchmark_std_regex(const std::string& pattern, const std::string& input) {
    std::regex re(pattern);
    
    // Warmup
    for (int i = 0; i < WARMUP; ++i) {
        std::regex_search(input, re);
    }
    
    // Benchmark
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        std::regex_search(input, re);
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / ITERATIONS;
}

// Run a single benchmark case
BenchResult run_benchmark(const std::string& name, const std::string& pattern, 
                          const std::string& input, const std::string& std_pattern = "") {
    BenchResult result;
    result.name = name;
    result.pattern = pattern;
    result.input = input.length() > 20 ? input.substr(0, 17) + "..." : input;
    
    result.jit_ns = benchmark_jit(pattern, input);
    
    // Use std_pattern if provided (for patterns that differ between JIT and std::regex)
    const std::string& p = std_pattern.empty() ? pattern : std_pattern;
    result.std_ns = benchmark_std_regex(p, input);
    
    result.speedup = result.jit_ns > 0 ? (double)result.std_ns / result.jit_ns : 0;
    
    return result;
}

void print_results(const std::vector<BenchResult>& results) {
    std::cout << "\n";
    std::cout << std::string(100, '=') << "\n";
    std::cout << "                           RegJIT vs std::regex Benchmark Results\n";
    std::cout << std::string(100, '=') << "\n";
    std::cout << std::left << std::setw(25) << "Test Case"
              << std::setw(20) << "Pattern"
              << std::right << std::setw(12) << "JIT (ns)"
              << std::setw(15) << "std::regex"
              << std::setw(12) << "Speedup"
              << "\n";
    std::cout << std::string(100, '-') << "\n";
    
    double total_speedup = 0;
    int count = 0;
    
    for (const auto& r : results) {
        std::cout << std::left << std::setw(25) << r.name
                  << std::setw(20) << r.pattern
                  << std::right << std::setw(12) << r.jit_ns
                  << std::setw(15) << r.std_ns
                  << std::setw(10) << std::fixed << std::setprecision(1) << r.speedup << "x"
                  << "\n";
        if (r.speedup > 0) {
            total_speedup += r.speedup;
            count++;
        }
    }
    
    std::cout << std::string(100, '-') << "\n";
    std::cout << std::left << std::setw(25) << "Average Speedup:"
              << std::right << std::setw(67) << std::fixed << std::setprecision(1) 
              << (count > 0 ? total_speedup / count : 0) << "x\n";
    std::cout << std::string(100, '=') << "\n";
}

int main() {
    std::vector<BenchResult> results;
    
    std::cout << "Running RegJIT vs std::regex benchmarks...\n";
    std::cout << "Iterations per test: " << ITERATIONS << "\n";
    
    // ========== Basic Character Matching ==========
    std::cout << "\n[1/8] Basic character matching..." << std::flush;
    results.push_back(run_benchmark(
        "Simple literal",
        "hello",
        "hello world"
    ));
    std::cout << " done\n";
    
    std::cout << "[2/8] Long literal matching..." << std::flush;
    results.push_back(run_benchmark(
        "Long literal",
        "abcdefghij",
        "xxxxxxxxxxabcdefghijyyyyyyyyyy"
    ));
    std::cout << " done\n";
    
    // ========== Quantifiers ==========
    std::cout << "[3/8] Quantifier benchmarks..." << std::flush;
    
    // a{1000} matching 1000 'a's
    std::string long_a(1000, 'a');
    results.push_back(run_benchmark(
        "Exact repeat {1000}",
        "a{1000}",
        long_a
    ));
    
    // a+ matching many 'a's
    results.push_back(run_benchmark(
        "Plus quantifier a+",
        "a+",
        long_a
    ));
    
    // a* matching many 'a's
    results.push_back(run_benchmark(
        "Star quantifier a*",
        "a*",
        long_a
    ));
    std::cout << " done\n";
    
    // ========== Character Classes ==========
    std::cout << "[4/8] Character class benchmarks..." << std::flush;
    
    std::string alphanum = "abc123XYZ789def456GHI";
    results.push_back(run_benchmark(
        "Char class [a-z]+",
        "[a-z]+",
        alphanum
    ));
    
    results.push_back(run_benchmark(
        "Char class [a-zA-Z0-9]+",
        "[a-zA-Z0-9]+",
        alphanum
    ));
    
    results.push_back(run_benchmark(
        "Negated class [^0-9]+",
        "[^0-9]+",
        alphanum
    ));
    std::cout << " done\n";
    
    // ========== Escape Sequences ==========
    std::cout << "[5/8] Escape sequence benchmarks..." << std::flush;
    
    std::string digits = "1234567890";
    std::string words = "hello_world_123";
    std::string mixed = "  \t\n  text  \r\n  ";
    
    results.push_back(run_benchmark(
        "Digit \\d+",
        "\\d+",
        digits
    ));
    
    results.push_back(run_benchmark(
        "Word \\w+",
        "\\w+",
        words
    ));
    
    results.push_back(run_benchmark(
        "Whitespace \\s+",
        "\\s+",
        mixed
    ));
    std::cout << " done\n";
    
    // ========== Alternation ==========
    std::cout << "[6/8] Alternation benchmarks..." << std::flush;
    
    results.push_back(run_benchmark(
        "Simple alternation",
        "cat|dog|bird",
        "I have a dog"
    ));
    
    results.push_back(run_benchmark(
        "Complex alternation",
        "hello|world|foo|bar|baz",
        "the world is beautiful"
    ));
    std::cout << " done\n";
    
    // ========== Anchors ==========
    std::cout << "[7/8] Anchor benchmarks..." << std::flush;
    
    results.push_back(run_benchmark(
        "Start anchor ^hello",
        "^hello",
        "hello world"
    ));
    
    results.push_back(run_benchmark(
        "End anchor world$",
        "world$",
        "hello world"
    ));
    
    results.push_back(run_benchmark(
        "Both anchors ^...$",
        "^hello world$",
        "hello world"
    ));
    std::cout << " done\n";
    
    // ========== Complex Patterns ==========
    std::cout << "[8/8] Complex pattern benchmarks..." << std::flush;
    
    // Email-like pattern
    results.push_back(run_benchmark(
        "Email-like pattern",
        "[a-z]+@[a-z]+\\.[a-z]+",
        "contact user@example.com for info"
    ));
    
    // IP-like pattern (simplified)
    results.push_back(run_benchmark(
        "IP-like pattern",
        "\\d+\\.\\d+\\.\\d+\\.\\d+",
        "Server IP is 192.168.1.100"
    ));
    
    // Nested groups
    results.push_back(run_benchmark(
        "Nested groups",
        "(a(b(c)+)+)+",
        "abcbcabcbcbc"
    ));
    
    // Long input search
    std::string long_text(10000, 'x');
    long_text += "needle";
    results.push_back(run_benchmark(
        "Long input search",
        "needle",
        long_text
    ));
    std::cout << " done\n";
    
    // Print results
    print_results(results);
    
    return 0;
}
