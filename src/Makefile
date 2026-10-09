CXX      := g++
CXXFLAGS := -std=gnu++17 -O2

.PHONY: all run clean

all: sim

sim: scheduler.cpp ready_queue.cpp ready_queue.hpp libsim.so
	$(CXX) $(CXXFLAGS) -o $@ scheduler.cpp ready_queue.cpp -L. -lsim -Wl,-rpath,'$$ORIGIN'

run: sim
	./sim

clean:
	rm -f sim
