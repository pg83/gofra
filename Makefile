# Builds gofra2 (C++ data plane on top of std/) alongside the Go
# tree. Plain `make` builds inside the ix dev-cc env (set up by std's
# dev/run.sh, which exports CXXFLAGS / CPPFLAGS / LDFLAGS pointing
# at musl / libc++ / liburing / etc).

STDDIR ?= ../std
SRCS = $(wildcard *.cpp)
OBJS = $(SRCS:%.cpp=%.cpp.o)

OPTF = -O2 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
CXXF = -I. -I$(STDDIR) -W -Wall -std=c++26 $(OPTF) $(CPPFLAGS) $(CFLAGS) $(CXXFLAGS) $(EXTRA)

all: gofra2

gofra2: $(OBJS) $(STDDIR)/std/libstd.a
	$(CXX) $(OPTF) -o $@ $(OBJS) $(STDDIR)/std/libstd.a $(LDFLAGS)

%.cpp.o: %.cpp $(wildcard *.h) Makefile
	$(CXX) $(CXXF) -o $@ -c $<

clean:
	rm -f gofra2 $(OBJS)
