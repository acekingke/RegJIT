CXX = clang++
#DEBUG_FLAGS = -g
DEBUG_FLAGS =
CXXFLAGS =$(DEBUG_FLAGS) -std=c++17 -Wno-unknown-warning-option $(shell llvm-config --cxxflags)
LDFLAGS = $(shell llvm-config --ldflags)
LDLIBS = $(shell llvm-config --libs core)

SRC = src/test.cpp src/regjit.cpp
OBJ = $(SRC:.cpp=.o)

# Consolidated test target
test: $(OBJ)
	$(CXX) $(DEBUG_FLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS) 


bench: src/benchmark.cpp src/regjit.o
	$(CXX) $(DEBUG_FLAGS) $(CXXFLAGS) -o $@ $^  $(LDFLAGS) $(LDLIBS) 

sample: src/sample.cpp src/regjit.o
	$(CXX) $(DEBUG_FLAGS) $(CXXFLAGS) -o $@ $^  $(LDFLAGS) $(LDLIBS) 
clean:
	rm -rf test*  sample bench src/*.o