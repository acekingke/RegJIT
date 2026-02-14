#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Match result structure - compatible with Python re.Match
typedef struct {
    int matched;    // 1 if matched, 0 if not matched, -1 on error
    int start;      // start position of match (-1 if no match)
    int end;        // end position of match (-1 if no match)
} regjit_match_result;

// Minimal C API for RegJIT
// Returns 1 on success, 0 on failure. On failure err_msg may be allocated with strdup.
int regjit_compile(const char* pattern, char** err_msg);

// Acquire a compiled pattern (increments refcount). Compiles if needed.
// Returns 1 on success, 0 on failure. On failure err_msg may be set (strdup).
int regjit_acquire(const char* pattern, char** err_msg);

// Release a previously acquired pattern (decrements refcount).
void regjit_release(const char* pattern);

// Match: returns 1 on match, 0 on no match, -1 on error (legacy API)
int regjit_match(const char* pattern, const char* buf, size_t len);

// re.match() compatible: Try to match pattern at the beginning of the string
// Returns match result with start/end positions
regjit_match_result regjit_match_at_start(const char* pattern, const char* buf, size_t len);

// re.search() compatible: Search for pattern anywhere in the string
// Returns match result with start/end positions
regjit_match_result regjit_search(const char* pattern, const char* buf, size_t len);

// Unload compiled pattern (free resources)
void regjit_unload(const char* pattern);

// Cache helpers
size_t regjit_cache_size();
void regjit_set_cache_maxsize(size_t n);

#ifdef __cplusplus
}
#endif
