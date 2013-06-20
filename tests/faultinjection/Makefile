
LIBIPQ_INCLUDES := -I.
LIBIPQ_SOURCE = libipq.c libipq/libipq.h 
LIBIPQ_SVN=http://svn.netfilter.org/netfilter/trunk/iptables

CC := gcc
CFLAGS := -Wall -O2 -m32
CPPFLAGS := $(LIBIPQ_INCLUDES)
LDFLAGS := -static -m32

PROGRAMS = pfilter

all: $(PROGRAMS)


pfilter: pfilter.o libipq.o
	$(CC) $^ $(LDFLAGS) -o $@

pfilter.c: $(LIBIPQ_SOURCE)

libipq.o: $(LIBIPQ_SOURCE)


libipq.c:
	wget -q -O$@ $(LIBIPQ_SVN)/libipq/libipq.c

libipq/libipq.h:
	-[ -d libipq ] || mkdir libipq
	wget -q -O$@ $(LIBIPQ_SVN)/include/libipq/libipq.h

clean:
	rm -f $(PROGRAMS) *.o
	rm -f *~
	rm -f $(LIBIPQ_SOURCE)
