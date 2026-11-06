CXX ?= g++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined

test: test_fault_logic
	./test_fault_logic

test_fault_logic: test_fault_logic.cpp ../../components/fault_monitor/fault_logic.h
	$(CXX) $(CXXFLAGS) -o $@ test_fault_logic.cpp

clean:
	rm -f test_fault_logic

.PHONY: test clean
