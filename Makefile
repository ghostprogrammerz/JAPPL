CC      = gcc
CFLAGS  = -Wall -Wextra -g -std=c11
SRC     = main.c lexer.c ast.c parser.c emitter.c decompiler.c
OBJ     = $(SRC:.c=.o)
TARGET  = jappl2sb3

SRV_SRC = server.c lexer.c ast.c parser.c emitter.c decompiler.c
SRV_OBJ = $(SRV_SRC:.c=.o)
SRV     = jappl2sb3-server

all: $(TARGET) $(SRV)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lz

$(SRV): $(SRV_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lz

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(SRV_OBJ) $(TARGET) $(SRV)

.PHONY: all clean
