#include "regjit.h"
#include <iostream>
#include <chrono>
#include <regex>
uint64_t benchmark_jit_repeat(int repeat_count, const char* input) {
    auto repeatBody = std::make_unique<Match>('a');
    auto repeatInst = std::make_unique<Repeat>(std::move(repeatBody), repeat_count);
    
    auto FunBlock = std::make_unique<Func>(std::move(repeatInst));
    FunBlock->CodeGen();
    Compile();
    
    auto MatchSym = ExitOnErr(JIT->lookup("match"));
    auto Func = (int (*)(const char*))MatchSym.getValue();

    // 预热运行
    Func(input);
    
    const int iterations = 100000;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        Func(input);
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    auto jit_time = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "JIT time (" << repeat_count << "): " 
              << jit_time / iterations << " ns/iter\n";
    return jit_time;
}

void benchmark_std_regex_repeat(int repeat_count, const char* input, uint64_t jit_time) {
    std::string pattern = "a{" + std::to_string(repeat_count) + "}";
    std::regex re(pattern);
    
    // 预热运行
    std::regex_match(input, re);
    
    const int iterations = 100000;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        std::regex_match(input, re);
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    auto std_time = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "std::regex time (" << repeat_count << "): " 
              << std_time / iterations << " ns/iter"
              << " (Speedup: " << std_time / jit_time << "x)\n";
}

void run_benchmarks() {
    const int test_cases[] = {1};
    const int str_len = 1000;
    std::string test_str(str_len, 'a'); // 全匹配字符串
    const int n = 1000;
    std::cout << "\nTesting a{" << n << "} on " << str_len << " chars:\n";
    auto jit_time = benchmark_jit_repeat(n, test_str.c_str());
    benchmark_std_regex_repeat(n, test_str.c_str(), jit_time);
    //   // 测试不匹配情况
    //   
    // TOFIX:需要clean up 
    // std::string mismatch_str = std::string(str_len-1, 'a') + "b";
    // std::cout << "\nTesting mismatch case (a{" << str_len << "} with last char 'b'):\n";
    // auto jit_time = benchmark_jit_repeat(n, mismatch_str.c_str());
    // benchmark_std_regex_repeat(n,  mismatch_str.c_str(), jit_time);
}
int main() {
    Initialize();
    run_benchmarks();
    return 0;
}
