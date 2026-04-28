# Builds gofra (C++ data plane on top of std/). Plain `make` works
# inside the ix dev-cc env (std's dev/run.sh exports CXXFLAGS /
# CPPFLAGS / LDFLAGS pointing at musl / libc++ / liburing / etc).
# For local dev: STDDIR points at the std/ source tree (sibling).
# For the ix build: lib/std drops its own -I/-L into the env.
STDDIR ?= ../std
SRCS = $(wildcard *.cpp)
OBJS = $(SRCS:%.cpp=%.cpp.o)

OPTF = -O2 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
CXXF = -I. -I$(STDDIR) -W -Wall -std=c++26 $(OPTF) $(CPPFLAGS) $(CFLAGS) $(CXXFLAGS) $(EXTRA)
LDF  = -L$(STDDIR)/std $(LDFLAGS) -lstd -lmnl

all: gofra

gofra: $(OBJS)
	$(CXX) $(OPTF) -o $@ $(OBJS) $(LDF)

%.cpp.o: %.cpp $(wildcard *.h) Makefile
	$(CXX) $(CXXF) -o $@ -c $<

clean:
	rm -f gofra $(OBJS)
