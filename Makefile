# Builds gofra2 (C++ data plane on top of std/) alongside the Go
# tree. Plain `make` builds inside the ix dev-cc env (set up by std's
# dev/run.sh, which exports CXXFLAGS / CPPFLAGS / LDFLAGS pointing
# at musl / libc++ / liburing / etc).

# For local dev: STDDIR points at the std/ source tree (sibling to
# gofra/), so a plain `cd ../std && make` followed by `cd ../gofra
# && make` builds gofra2 against the just-compiled libstd.a.
#
# For the ix build: lib/std drops its own -I/-L into CPPFLAGS/LDFLAGS,
# so the ix-injected paths win and STDDIR is unused.
STDDIR ?= ../std
SRCS = $(wildcard *.cpp)
OBJS = $(SRCS:%.cpp=%.cpp.o)

OPTF = -O2 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
CXXF = -I. -I$(STDDIR) -W -Wall -std=c++26 $(OPTF) $(CPPFLAGS) $(CFLAGS) $(CXXFLAGS) $(EXTRA)
LDF  = -L$(STDDIR)/std $(LDFLAGS) -lstd

all: gofra2

gofra2: $(OBJS)
	$(CXX) $(OPTF) -o $@ $(OBJS) $(LDF)

%.cpp.o: %.cpp $(wildcard *.h) Makefile
	$(CXX) $(CXXF) -o $@ -c $<

clean:
	rm -f gofra2 $(OBJS)
