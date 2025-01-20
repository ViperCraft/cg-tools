CC=gcc
CFLAGS=-O2 -g0

tools: $(CC) -o $@ $^ $(CFLAGS)
