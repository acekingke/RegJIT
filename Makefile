DEBUG_FLAGS := -g -O0
LLVM_CONFIG := /opt/homebrew/opt/llvm/bin/llvm-config
CXXFLAGS := $(DEBUG_FLAGS) -Wall -Wno-unknown-warning-option -std=c++17
CXXFLAGS += $(shell $(LLVM_CONFIG) --cxxflags)

LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags)
LDLIBS   := $(shell $(LLVM_CONFIG) --libs core)

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
test_charclass: tests/test_charclass.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

debug: tests/debug.cpp $(REGJIT_OBJ)
	$(CXX) $(CXXFLAGS) -I./src -o $@ $^ $(LDFLAGS) $(LDLIBS)

simple_charclass: tests/simple_charclass.cpp $(REGJIT_OBJ)
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

# Run all tests in tests directory
test_all: test_charclass
	@echo "Running all tests in tests/ directory..."
	@if [ -f test_charclass ]; then ./test_charclass; fi
	@if [ -f test_anchor ]; then ./test_anchor; fi
	@if [ -f test_quantifier ]; then ./test_quantifier; fi

bench: src/benchmark.cpp src/regjit.o
	$(CXX) $(CXXFLAGS) -o $@ $^  $(LDFLAGS) $(LDLIBS) 

sample: src/sample.cpp src/regjit.o
	$(CXX) $(CXXFLAGS) -o $@ $^  $(LDFLAGS) $(LDLIBS) 

clean:
	rm -rf test_* sample bench src/*.o

# Clean only compiled test executables, keep source files
clean_tests:
	rm -f test_charclass test_anchor test_quantifier debug simple_charclass test_cleanup