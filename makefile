CXX      = g++
CXXFLAGS = -std=c++17 -O3 -march=native -Wall -Wextra -I include

all: phase1 phase2 phase3 phase4

phase1: src/main.cpp include/types.h src/synthetic.h src/csv_reader.h
	$(CXX) $(CXXFLAGS) src/main.cpp -o phase1

phase2: src/test_orderbook.cpp include/types.h src/orderbook.h
	$(CXX) $(CXXFLAGS) src/test_orderbook.cpp -o phase2

phase3: src/bench_runner.cpp include/types.h src/orderbook.h \
        src/price_ladder.h src/bench.h src/synthetic.h
	$(CXX) $(CXXFLAGS) src/bench_runner.cpp -o phase3

phase4: src/pipeline.cpp include/types.h src/csv_reader.h src/price_ladder.h
	$(CXX) $(CXXFLAGS) src/pipeline.cpp -o phase4

clean:
	rm -f phase1 phase2 phase3 phase4