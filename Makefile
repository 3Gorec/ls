BUILD_DIR=./
INCLUDES=
LDFLAGS=
TARGET=ls

CFLAGS+=-g
SOURCES=ls.c

all:
		$(CC) $(CFLAGS) $(SOURCES) -o ./ls  $(INCLUDES) $(LDFLAGS)