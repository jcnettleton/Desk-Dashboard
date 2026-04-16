CXX      := g++
GFX_DIR  := ../.pio/libdeps/esp32s3/Adafruit GFX Library
CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unused-variable -Wno-sign-compare -DARDUINO=100
TARGET   := sim

.PHONY: all clean

all: $(TARGET)

$(TARGET): sim_main.cpp Arduino.h Print.h
	$(CXX) $(CXXFLAGS) -I. -I"$(GFX_DIR)" -o $@ sim_main.cpp "$(GFX_DIR)/Adafruit_GFX.cpp"

clean:
	rm -f $(TARGET)
