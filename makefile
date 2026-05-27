CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic -I include

SRC      = src/main.cpp
TARGET   = phase1

all: $(TARGET)

$(TARGET): $(SRC) include/types.h src/synthetic.h src/csv_reader.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)