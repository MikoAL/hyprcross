# Build against the installed Hyprland headers; the .so only loads into the exact
# Hyprland it was built for, so rebuild after every Hyprland update.
# -fno-gnu-unique lets `hyprctl plugin unload` really unload it, so a rebuild can be loaded live.
CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -shared -fPIC -fno-gnu-unique -std=c++2c -Wall -Wextra -Wno-unused-parameter
PKGS      = hyprland pixman-1 libdrm lua5.5
PREFIX   ?= $(HOME)/.local/lib/hyprland

hyprcross.so: src/main.cpp
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags $(PKGS)) $< -o $@

all: hyprcross.so

install: hyprcross.so
	install -Dm755 hyprcross.so $(PREFIX)/hyprcross.so

# Swap the running copy for the fresh build without restarting Hyprland.
reload: install
	-hyprctl plugin unload $(PREFIX)/hyprcross.so
	hyprctl plugin load $(PREFIX)/hyprcross.so

clean:
	rm -f hyprcross.so

.PHONY: all install reload clean
