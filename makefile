CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic -I include

all: phase1 phase2

phase1: src/main.cpp include/types.h src/synthetic.h src/csv_reader.h
	$(CXX) $(CXXFLAGS) src/main.cpp -o phase1

phase2: src/test_orderbook.cpp include/types.h src/orderbook.h
	$(CXX) $(CXXFLAGS) src/test_orderbook.cpp -o phase2

clean:
	rm -f phase1 phase2