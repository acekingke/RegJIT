#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include "../src/regjit_capi.h"

int main() {
    const char* pattern = "concurrent";
    char* err = nullptr;
    regjit_set_cache_maxsize(16);

    const int N = 32;
    std::atomic<int> success_count{0};
    std::vector<std::thread> ths;
    ths.reserve(N);

    for (int i = 0; i < N; ++i) {
        ths.emplace_back([&success_count, pattern]() {
            char* err = nullptr;
            if (regjit_acquire(pattern, &err)) {
                success_count.fetch_add(1);
            } else {
                if (err) { fprintf(stderr, "acquire error: %s\n", err); free(err); }
            }
            // hold the reference briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            regjit_release(pattern);
        });
    }

    for (auto &t : ths) t.join();

    if ((int)success_count != N) {
        fprintf(stderr, "concurrent acquire failed: %d/%d succeeded\n", (int)success_count.load(), N);
        return 2;
    }

    size_t sz = regjit_cache_size();
    if (sz < 1) {
        fprintf(stderr, "concurrent test: expected cache size >=1, got %zu\n", sz);
        return 3;
    }

    printf("concurrent acquire test passed\n");
    return 0;
}
