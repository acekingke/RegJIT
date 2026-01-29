#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal C API for RegJIT
// Returns 1 on success, 0 on failure. On failure err_msg may be allocated with strdup.
int regjit_compile(const char* pattern, char** err_msg);

// Match: returns 1 on match, 0 on no match, -1 on error
int regjit_match(const char* pattern, const char* buf, size_t len);

// Unload compiled pattern (free resources)
void regjit_unload(const char* pattern);

#ifdef __cplusplus
}
#endif
