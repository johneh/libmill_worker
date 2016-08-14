CC = gcc
CFLAGS = -c -O2 -g -I../ -pthread
BINS = apache_serve fsop pi mcp du3 xchoose fanin wrker mu
LIBS = ../lib/libmill.a -pthread

all: clean $(BINS)

apache_serve: apache_serve.o
	$(CC) -o $@ $^ $(LIBS)

fsop: fsop.o
	$(CC) -o $@ $^ $(LIBS)

xchoose: xchoose.o
	$(CC) -o $@ $^ $(LIBS)

fanin: fanin.o
	$(CC) -o $@ $^ $(LIBS)

pi: pi.o
	$(CC) -o $@ $^ $(LIBS) -lm

wrker: wrker.o
	$(CC) -o $@ $^ $(LIBS)

du3: du3.o readdir.o
	$(CC) -o $@ $^ $(LIBS)

du3.o: du3.c readdir.h
	$(CC) $(CFLAGS) du3.c

readdir.o: readdir.h readdir.c
	$(CC) $(CFLAGS) readdir.c

mcp: mcp.o
	$(CC) -o $@ $^ $(LIBS)

mu: mu.o
	$(CC) -o $@ $^ $(LIBS)

.c.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f *.o
	rm -f $(BINS)
