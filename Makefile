CC = gcc
CFLAGS = -Wall -Wextra -g -std=c11
LDFLAGS = -lz

TARGET = jappl2sb3

SOURCES = main.c lexer.c ast.c parser.c emitter.c decompiler.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET) server

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

server: server.o $(OBJECTS)
	$(CC) $(CFLAGS) -o jappl-server $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

server.o: server.c
	$(CC) $(CFLAGS) -Icompiler -Idecompiler -c $< -o $@

clean:
	rm -f $(OBJECTS) server.o $(TARGET) jappl-server

.PHONY: all clean
