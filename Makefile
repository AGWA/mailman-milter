CXX := c++
CXXFLAGS := -Wall -pedantic -ansi -Wno-long-long -O2
LDFLAGS := -L/usr/lib/libmilter -lmilter -lpthread
PREFIX := /usr/local

PROGRAMS = mailman-milter
OBJFILES = mailman-milter.o utils.o

all: $(PROGRAMS)

mailman-milter: $(OBJFILES)
	$(CXX) $(CXXLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f *.o $(PROGRAMS)

install:
	install -m 755 $(PROGRAMS) $(PREFIX)/bin/

.PHONY: all clean install
