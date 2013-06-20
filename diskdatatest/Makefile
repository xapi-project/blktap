# 
# XenRT: Disk test utility
#
# Julian Chesterfield, July 2007
#
# Copyright (c) 2007 XenSource, Inc. All use and distribution of this
# copyrighted material is governed by and subject to terms and
# conditions as licensed by XenSource, Inc. All other rights reserved.
#

TARGET = diskdatatest
BUILDFILES := atomicio.c
BUILDFILES += fdpattern.c
BUILD_OPTS := -D_GNU_SOURCE
BUILD_OPTS += -D_FILE_OFFSET_BITS=64
BUILD_OPTS += -D_LARGEFILE_SOURCE
BUILD_OPTS += -D_LARGEFILE64_SOURCE

#include ../config.mk 

.PHONY: all
all: $(TARGET)

$(TARGET): $(BUILDFILES)
	$(CC) $(CCOPTS) $(BUILD_OPTS) -o $@ $(BUILDFILES)

clean:
	rm -f $(TARGET) *.o

