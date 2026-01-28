---
name: regjit-makefile
description: Use when maintaining Makefile, adding build targets, managing LLVM configuration, and optimizing build process
---

# RegJIT Makefile Management

## Overview

Maintain clear and efficient Makefile for building RegJIT project, managing test targets, and handling LLVM integration.

## When to Use

- Adding new test targets
- Changing build flags or compiler options
- Updating LLVM dependencies
- Optimizing build performance
- Documenting build procedures
- Setting up CI/CD integration

## Makefile Structure

### Essential Sections

#### 1. Configuration
```makefile
DEBUG_FLAGS := -g -O0
LLVM_CONFIG := /opt/homebrew/Cellar/llvm/19.1.7_1/bin/llvm-config
CXXFLAGS := $(DEBUG_FLAGS) -Wall -Wno-unknown-warning-option -std=c++17
CXXFLAGS += $(shell $(LLVM_CONFIG) --cxxflags)
LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags)
LDLIBS := $(shell $(LLVM_CONFIG) --libs)
```

**Purpose:** Centralize build configuration
- Single source of truth for flags
- Dynamic LLVM configuration
- Easy cross-platform adaptation

#### 2. File Variables
```makefile
REGJIT_OBJ := src/regjit.o
REGJIT_SRC := src/regjit.cpp src/regjit.h
```

**Purpose:** Avoid duplication, improve maintainability

#### 3. Phony Targets
```makefile
.PHONY: all clean test test_all
```

**Purpose:** Distinguish logical targets from real files

#### 4. Build Rules
```makefile
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

test_anchor: tests/test_anchor.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)
```

**Purpose:** Clear dependency management

## Adding New Test Target

### Template
```makefile
test_new_feature: tests/test_new_feature.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)
```

### Steps
1. Create test file in `tests/test_new_feature.cpp`
2. Add target to Makefile
3. Update `test_all` dependency list
4. Test: `make test_new_feature && ./test_new_feature`

### Example: Adding anchor_quant_edge test
```makefile
test_anchor_quant_edge: tests/test_anchor_quant_edge.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)
```

Then add to test_all:
```makefile
test_all: test_anchor test_charclass test_quantifier test_anchor_quant_edge
	@echo "Running all tests in tests/ directory..."
```

## Common Makefile Patterns

### Implicit Rules (Pattern Matching)
```makefile
# Compile all .cpp files to .o
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
```

**Benefit:** Automatically applies to new files

### Using Variables
```makefile
# Good - reusable
test_anchor: tests/test_anchor.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Bad - hardcoded, not maintainable
test_anchor: tests/test_anchor.cpp src/regjit.o
	c++ -g -O0 -Wall -std=c++17 tests/test_anchor.cpp src/regjit.o -o test_anchor
```

### Dependency Tracking
```makefile
# Object file depends on source files
src/regjit.o: src/regjit.cpp src/regjit.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Test executable depends on test source and core object
test: test.cpp src/regjit.o
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)
```

## Variable Reference

### Automatic Variables
| Variable | Meaning |
|----------|---------|
| `$@` | Target name (e.g., test_anchor) |
| `$<` | First dependency (e.g., test_anchor.cpp) |
| `$^` | All dependencies |
| `$+` | All dependencies (with duplicates) |

### LLVM Variables
```makefile
CXXFLAGS += $(shell $(LLVM_CONFIG) --cxxflags)
LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags)
LDLIBS := $(shell $(LLVM_CONFIG) --libs)
```

## Common Issues & Solutions

### Issue: Rebuilding Everything Unnecessarily
**Problem:** Missing dependencies cause full rebuild
**Solution:** List all file dependencies correctly
```makefile
# WRONG - recompiles even if header doesn't change
test: test.cpp
	$(CXX) ... -o $@ $^

# RIGHT - recompiles only on actual changes
test: test.cpp src/regjit.o
	$(CXX) ... -o $@ $^ $(LDLIBS)
```

### Issue: Hardcoded Paths Break on Different Systems
**Problem:** `/usr/local/bin/llvm-config` doesn't exist everywhere
**Solution:** Use dynamic discovery
```makefile
# WRONG
LLVM_CONFIG := /usr/local/bin/llvm-config

# GOOD - uses which to find it
LLVM_CONFIG := $(shell which llvm-config)
```

### Issue: Phony Targets Treated as Files
**Problem:** `make clean` creates empty file named "clean"
**Solution:** Declare phony targets
```makefile
.PHONY: clean test all
clean:
	rm -f *.o test_* debug*
```

### Issue: Parallel Build Fails
**Problem:** `make -j4` breaks with incorrect dependencies
**Solution:** Ensure all dependencies listed
```makefile
# Good for parallel
test_anchor: tests/test_anchor.cpp $(REGJIT_OBJ)
test_charclass: tests/test_charclass.cpp $(REGJIT_OBJ)
# Can run in parallel without issues
```

## Build Commands

### Common Operations
```bash
# Default build
make

# Specific test
make test_anchor

# All tests
make test_all

# Clean artifacts
make clean

# Rebuild everything
make clean && make

# Parallel build (4 cores)
make -j4

# Show what would be executed (dry run)
make -n test_anchor

# Debug dependencies
make -d test_anchor
```

### Checking Makefile Correctness
```bash
# Syntax check
make -n all    # Shows commands without executing

# Verify dependencies
make -d 2>&1 | grep "test_anchor"

# Clean rebuild
make clean && make test_all
```

## Optimization Tips

### 1. Object File Caching
```makefile
src/regjit.o: src/regjit.cpp src/regjit.h
	$(CXX) $(CXXFLAGS) -c $< -o $@
```
Benefits: Only recompile when source changes

### 2. Parallel Builds
```bash
make -j4  # Use 4 cores
```
Speedup: 3-4x faster on multi-core machines

### 3. Conditional Compilation
```makefile
ifdef DEBUG
    CXXFLAGS += -DDEBUG
endif
```
Usage: `make DEBUG=1`

## Maintenance Checklist

- [ ] All source files listed in dependencies
- [ ] No circular dependencies
- [ ] All targets have prerequisites
- [ ] Phony targets declared (.PHONY)
- [ ] Variables centralized at top
- [ ] Comments explain non-obvious rules
- [ ] Clean target removes all artifacts
- [ ] Build reproducible (same output)
- [ ] No hardcoded paths
- [ ] Works with `make -j`

## Viewing Makefile

```bash
# Show all targets
grep "^[a-z_]*:" Makefile

# Show variables
grep "^[A-Z_]* :=" Makefile

# Show dependencies for target
make -d test_anchor 2>&1 | grep "test_anchor"
```

## Quick Reference

| Command | Effect |
|---------|--------|
| `make` | Build default (all) |
| `make clean` | Remove artifacts |
| `make test_X` | Build test_X |
| `make -n` | Show commands (no execute) |
| `make -j N` | Parallel with N cores |
| `make -d` | Debug mode |
