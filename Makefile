DEBUG_FLAGS := -g -O0
LLVM_CONFIG := /opt/homebrew/Cellar/llvm/19.1.7_1/bin/llvm-config
CXXFLAGS := $(DEBUG_FLAGS) -Wall -Wno-unknown-warning-option -std=c++17
CXXFLAGS += $(shell $(LLVM_CONFIG) --cxxflags)

LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags)
LDLIBS   := $(shell $(LLVM_CONFIG) --libs core)

# pybind11 settings
PYBIND11_INCLUDE := -I/usr/local/include/pybind11
# Use python3.12 for building python bindings
PYTHON_BIN := python3.12
PYTHON_INCLUDES := -I$(shell $(PYTHON_BIN) -c "import sysconfig; p=sysconfig.get_paths(); print(p['include'])")

# Build shared lib for regjit core
libregjit.so: src/regjit.o
	$(CXX) -shared -fPIC -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Python extension module (_regjit) using pybind11
python/_regjit.so: libregjit.so python/bindings.cpp
	$(CXX) $(CXXFLAGS) $(PYBIND11_INCLUDE) $(PYTHON_INCLUDES) -I./src -fPIC -shared -o $@ python/bindings.cpp libregjit.so $(LDFLAGS) $(LDLIBS) -undefined dynamic_lookup

.PHONY: python-bindings
python-bindings: python/_regjit.so
	@echo "Built python bindings: python/_regjit.so"

# Source and object files
SRC = src/test.cpp src/regjit.cpp
OBJ = $(SRC:.cpp=.o)
REGJIT_OBJ = src/regjit.o
# 编译规则
src/%.o: src/%.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 链接规则（Debug）
test: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Test specific functionality in tests/
test_anchor_quant_edge: tests/test_anchor_quant_edge.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)
test_charclass: tests/test_charclass.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

debug: tests/debug.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_cleanup: tests/test_cleanup.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_charclass_only: tests/test_charclass_only.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_simple_charclass: tests/test_simple_charclass.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_anchor: tests/test_anchor.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_quantifier: tests/test_quantifier.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

simple_anchor_test: tests/simple_anchor_test.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

debug_test: debug_test.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

debug_test2: debug_test2.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

final_test: final_test.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

simple_test: simple_test.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

debug_dollar: debug_dollar.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

debug_engine: debug_engine.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_single: test_single.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_basic_multi: test_basic_multi.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_wrong: tests/test_wrong.cpp $(REGJIT_OBJ)
	$(CXX) -DREGJIT_DEBUG $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Run all tests in tests directory
test_all: test_charclass test_anchor test_quantifier test_anchor_quant_edge test_cleanup simple_anchor_test
	@echo "Running all tests in tests/ directory..."
	@if [ -f test_charclass ]; then echo "=== Running test_charclass ==="; ./test_charclass || echo "test_charclass failed"; fi
	@if [ -f test_anchor ]; then echo "=== Running test_anchor ==="; timeout 3 ./test_anchor || echo "test_anchor failed or timed out"; fi
	@if [ -f test_quantifier ]; then echo "=== Running test_quantifier ==="; timeout 15 ./test_quantifier || echo "test_quantifier failed or timed out"; fi
	@if [ -f test_anchor_quant_edge ]; then echo "=== Running test_anchor_quant_edge ==="; timeout 3 ./test_anchor_quant_edge || echo "test_anchor_quant_edge failed or timed out"; fi
	@if [ -f test_cleanup ]; then echo "=== Running test_cleanup ==="; ./test_cleanup || echo "test_cleanup failed"; fi
	@if [ -f simple_anchor_test ]; then echo "=== Running simple_anchor_test ==="; ./simple_anchor_test || echo "simple_anchor_test failed"; fi
	@echo "All tests completed!"

bench: src/benchmark.cpp src/regjit.o
	$(CXX) $(CXXFLAGS) -o $@ $^  $(LDFLAGS) $(LDLIBS) 

sample: src/sample.cpp src/regjit.o
	$(CXX) $(CXXFLAGS) -o $@ $^  $(LDFLAGS) $(LDLIBS) 

clean:
	rm -rf test_* sample bench src/*.o debug_test* final_test

# Clean only compiled test executables, keep source files

# Clean only compiled test executables, keep source files
clean_tests:
	rm -f test_charclass test_anchor test_quantifier test_anchor_quant_edge test_cleanup test_charclass_only test_simple_charclass debug simple_anchor_test simple_test final_test

# Quick test - run only main functionality tests
test_quick: test_anchor test_charclass test_quantifier
	@echo "Running quick tests (main functionality)..."
	@if [ -f test_anchor ]; then echo "=== Running test_anchor ==="; timeout 3 ./test_anchor || echo "test_anchor failed or timed out"; fi
	@if [ -f test_charclass ]; then echo "=== Running test_charclass ==="; ./test_charclass || echo "test_charclass failed"; fi
	@if [ -f test_quantifier ]; then echo "=== Running test_quantifier ==="; timeout 15 ./test_quantifier || echo "test_quantifier failed or timed out"; fi
	@echo "Quick tests completed!"

# Help target
help:
	@echo "Available targets:"
	@echo "  test          - Build basic test (src/test.cpp)"
	@echo "  test_all      - Build and run all tests"
	@echo "  test_quick    - Build and run main functionality tests only"
	@echo "  test_*        - Build specific test (e.g., test_anchor, test_charclass)"
	@echo "  clean         - Remove all compiled files"
	@echo "  clean_tests   - Remove test executables only"
	@echo "  bench         - Build benchmark"
	@echo "  sample        - Build sample"
	@echo "  help          - Show this help"
