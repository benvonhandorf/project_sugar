CXX = g++
CXXFLAGS = -g  -std=c++11 -Wall -O2 -pthread -I/sw/include/root -I/usr/include/cpp `pkg-config --cflags opencv4` `pkg-config --cflags gstreamer-1.0`
LDFLAGS = -g -L /usr/lib
LDLIBS = -lArduCamLib -lArducamDepthCamera -lstdc++ `pkg-config --libs opencv4` `pkg-config --libs gstreamer-1.0`

TARGET = tof_camera
OBJS= tof_camera.o gst_target.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $< 

.PHONY: all clean

clean:
	rm *.o tof_camera