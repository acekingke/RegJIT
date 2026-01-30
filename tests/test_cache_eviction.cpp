#include <cstdio>
#include <cstdlib>
#include <cassert>
#include "../src/regjit_capi.h"

int main() {
    char* err = nullptr;
    // Test 1: basic eviction keeps cache at max size, evicting oldest unreferenced
    regjit_set_cache_maxsize(2);
    if (!regjit_acquire("a", &err)) { fprintf(stderr, "acquire a failed: %s\n", err); return 2; }
    regjit_release("a");
    if (!regjit_acquire("b", &err)) { fprintf(stderr, "acquire b failed: %s\n", err); return 2; }
    regjit_release("b");
    if (!regjit_acquire("c", &err)) { fprintf(stderr, "acquire c failed: %s\n", err); return 2; }
    regjit_release("c");
    size_t sz = regjit_cache_size();
    if (sz != 2) {
        fprintf(stderr, "test_basic_eviction failed: expected cache size 2, got %zu\n", sz);
        return 3;
    }

    // Test 2: referenced entries are not evicted even if over max
    regjit_set_cache_maxsize(1);
    if (!regjit_acquire("keep", &err)) { fprintf(stderr, "acquire keep failed: %s\n", err); return 4; }
    // add two other patterns (released) -> eviction should stop when encountering 'keep'
    if (!regjit_acquire("x", &err)) { fprintf(stderr, "acquire x failed: %s\n", err); return 4; }
    regjit_release("x");
    if (!regjit_acquire("y", &err)) { fprintf(stderr, "acquire y failed: %s\n", err); return 4; }
    regjit_release("y");

    sz = regjit_cache_size();
    if (sz <= 1) {
        fprintf(stderr, "test_referenced_prevents_eviction failed: expected cache size >1, got %zu\n", sz);
        return 5;
    }

    // Cleanup: release keep
    regjit_release("keep");

    printf("cache eviction tests passed\n");
    return 0;
}
