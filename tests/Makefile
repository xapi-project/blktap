# FIXME need to figure out how to statically compile biotest on 64 bit for 32
# bit.

all: biotest

biotest: biotest.c Makefile
	gcc -m32 -laio -Wall biotest.c -o biotest
