# Build against the installed Hyprland headers; the .so only loads into the exact
# Hyprland it was built for, so rebuild after every Hyprland update.
CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -shared -fPIC -std=c++2c -Wall -Wextra -Wno-unused-parameter
PKGS      = hyprland pixman-1 libdrm lua5.5
PREFIX   ?= $(HOME)/.local/lib/hyprcross

hyprcross.so: src/main.cpp
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags $(PKGS)) $< -o $@

all: hyprcross.so

install: hyprcross.so
	install -Dm755 hyprcross.so $(PREFIX)/hyprcross.so

clean:
	rm -f hyprcross.so

.PHONY: all install clean
