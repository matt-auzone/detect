CXX ?= g++
CXXFLAGS ?= -O3 -Wall
APP ?= detect
VERSION ?= -DVERSION=\""$(shell git describe || cat VERSION)\""

INC := -Iext/include
LIB := -lzmq -lvideostream -lvaal -ldeepview-rt

all: $(APP)

$(APP): detect.cpp
	$(CXX) $(CXXFLAGS) $(INC) $(VERSION) -o $(APP) detect.cpp $(LIB)

clean:
	$(RM) $(APP)
