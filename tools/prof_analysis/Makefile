# Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

CC=g++
CFLAGS=-c -Wall
INCDIRS=-I../../

all: prof_analysis

prof_analysis: tree.o main.o
	$(CC) tree.o main.o -o prof_analysis

tree.o:
	$(CC) $(CFLAGS) tree.cc

main.o:
	$(CC) $(CFLAGS) $(INCDIRS) main.cc

clean:
	rm -rf prof_analysis *.o

