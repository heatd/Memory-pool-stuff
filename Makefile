CC=gcc-9
CXX=g++-9

all:
	$(CXX) -o test test.cpp -std=c++2a -fconcepts -g -Og -fsanitize=undefined -fsanitize=address
