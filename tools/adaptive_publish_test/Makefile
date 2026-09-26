CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

all: test_runner
	./test_runner

test_runner: main.cpp ../../main/ws/include/adaptive_publish.h
	$(CXX) $(CXXFLAGS) main.cpp -o test_runner

clean:
	rm -f test_runner
