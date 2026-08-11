# Makefile for the camera-gui EGT + GStreamer application.
#
# Buildroot invokes this with CXX / CXXFLAGS / LDFLAGS / PKG_CONFIG / STAGING_DIR
# passed on the make command line (see TARGET_CONFIGURE_OPTS). Because those
# arrive as command-line variables they override plain and "+=" assignments in
# this file, so we use "override ... +=" to keep our own required flags.
#
# It also builds standalone on a host that has the libraries via pkg-config:
#     make            # cross or native, honouring $(CXX)/$(PKG_CONFIG)
#     make clean

TARGET     := AESDLinuxEgtProject
SRCS       := main.cpp

CXX        ?= g++
PKG_CONFIG ?= pkg-config

PKGS       := libegt gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0

# -pthread is required for the std::thread UDP sensor listener in main.cpp; it
# is added to both compile and link flags (the two go together for pthread).
override CXXFLAGS += -std=c++17 -O2 -pthread $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS     := $(shell $(PKG_CONFIG) --libs $(PKGS)) -pthread

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
