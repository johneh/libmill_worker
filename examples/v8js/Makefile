# V8 compiled using 'make i18nsupport=off x64.release'.
# With i18nsupport on, append third_part/icu/libicu{uc,i18n,data}.a
# to V8_LIBS

CC = gcc
CPP = g++
CFLAGS = -g -O2
CPPFLAGS = -g -O2
#CPPFLAGS = -g -O2 -DV8TEST=1
#LIBS = -L$(HOME)/opt/lib -lmill
LIBS = ../../lib/libmill.a

V8_DIR = $(HOME)/src/v8
V8_TARGET = x64.release
V8_INCLUDES = -I$(V8_DIR) -I$(V8_DIR)/include
V8_OUTDIR = $(V8_DIR)/out/$(V8_TARGET)
V8_LIBDIR = $(V8_OUTDIR)/obj.target/src
V8_LIBS = -Wl,--start-group $(V8_LIBDIR)/libv8_base.a \
$(V8_LIBDIR)/libv8_libbase.a $(V8_LIBDIR)/libv8_external_snapshot.a \
$(V8_LIBDIR)/libv8_libplatform.a -Wl,--end-group -lrt -ldl -pthread

INCLUDES = -I. -I../.. $(V8_INCLUDES)

BINS = testjs

all: $(BINS)

v8binding.o: v8binding.cc v8binding.h jsdef.h vm.h
	$(CPP) $(CPPFLAGS) $(INCLUDES) -DV8BINDIR="\"$(V8_OUTDIR)/\"" -std=c++0x -c v8binding.cc

jsv8.o: v8binding.h jsdef.h jsv8.h jsv8.c
	$(CC) $(CFLAGS) -Wall $(INCLUDES) -c jsv8.c

util.o: util.h util.c
	$(CC) $(CFLAGS) -Wall $(INCLUDES) -c util.c

libmilljs.a: v8binding.o jsv8.o util.o
	ar rcs $@ $^

testjs.o: jsv8.h testjs.c
	$(CC) $(CFLAGS) -Wall $(INCLUDES) -c testjs.c

testjs: testjs.o libmilljs.a
	$(CC) -o $@ $^ $(V8_LIBS) $(LIBS) -lm -lstdc++

clean:
	rm -f *.o
	rm -f $(BINS)
	rm -f libmilljs.a
