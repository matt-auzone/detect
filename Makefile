CXX ?= g++
CXXFLAGS ?= -O3 -Wall

all: detect

detect: detect.cpp
	$(CXX) $(CXXFLAGS) -o detect detect.cpp -lzmq -lvideostream -lvaal -ldeepview-rt

clean:
	$(RM) detect
