# Makefile for the camera-gui EGT application (Sprint 1: hello + button).
#
# Buildroot invokes this with CXX / CXXFLAGS / LDFLAGS / PKG_CONFIG / STAGING_DIR
# passed on the make command line (see TARGET_CONFIGURE_OPTS). Because those
# arrive as command-line variables they override plain and "+=" assignments in
# this file, so we use "override ... +=" to keep our own required flags.
#
# It also builds standalone on a host that has libegt via pkg-config:
#     make            # cross or native, honouring $(CXX)/$(PKG_CONFIG)
#     make clean

TARGET     := AESDLinuxEgtProject
SRCS       := main.cpp

CXX        ?= g++
PKG_CONFIG ?= pkg-config

# Sprint 1 only needs EGT. (Sprint 2 adds -pthread; Sprint 3 adds the
# gstreamer-1.0 / -app / -video packages.)
PKGS       := libegt

override CXXFLAGS += -std=c++17 -O2 $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS     := $(shell $(PKG_CONFIG) --libs $(PKGS))

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
