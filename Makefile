# Build against the installed Hyprland headers; the .so only loads into the exact
# Hyprland it was built for, so rebuild after every Hyprland update.
# -fno-gnu-unique lets `hyprctl plugin unload` really unload it, so a rebuild can be loaded live.
CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -shared -fPIC -fno-gnu-unique -std=c++2c -Wall -Wextra -Wno-unused-parameter
PKGS      = hyprland pixman-1 libdrm lua5.5
PREFIX   ?= $(HOME)/.local/lib/hyprland

SRCS = src/main.cpp src/overview.cpp

hyprcross.so: $(SRCS) src/shared.hpp src/overview.hpp
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags $(PKGS)) $(SRCS) -o $@

all: hyprcross.so

install: hyprcross.so
	install -Dm755 hyprcross.so $(PREFIX)/hyprcross.so

# Swap the running copy for the fresh build without restarting Hyprland.
reload: install
	-hyprctl plugin unload $(PREFIX)/hyprcross.so
	hyprctl plugin load $(PREFIX)/hyprcross.so

# Lua logic that doesn't need Hyprland (cross layout, gesture routing).
test:
	lua5.5 tests/cells_test.lua
	lua5.5 tests/gestures_test.lua
	lua5.5 tests/binds_test.lua

clean:
	rm -f hyprcross.so

.PHONY: all install reload test clean
