CXX ?= g++
CXXFLAGS ?= -O3 -Wall
VERSION ?= -DVERSION=\"$(shell git describe)\"

INC := -Iext/include
LIB := -lzmq -lvideostream -lvaal -ldeepview-rt

all: detect

detect: detect.cpp
	$(CXX) $(CXXFLAGS) $(INC) $(VERSION) -o detect detect.cpp $(LIB)

clean:
	$(RM) detect
