CXX      := g++
CXXFLAGS := -std=gnu++17 -O2

.PHONY: all run clean test benchmark placement-benchmark

all: sim

sim: scheduler.cpp scheduler.hpp job_placement.hpp ready_queue.cpp ready_queue.hpp libsim.so
	$(CXX) $(CXXFLAGS) -o $@ scheduler.cpp ready_queue.cpp -L. -lsim -Wl,-rpath,'$$ORIGIN'

run: sim
	./sim

test:
	CXX="$(CXX)" sh ../tests/run_benchmarks.sh --check

benchmark:
	CXX="$(CXX)" sh ../tests/run_benchmarks.sh --sweep

placement-benchmark:
	CXX="$(CXX)" sh ../tests/run_benchmarks.sh --placement-sweep

clean:
	rm -f sim
