CC      = gcc
CFLAGS  = -Wall -Wextra -g -std=c11
SRC     = main.c lexer.c ast.c parser.c emitter.c decompiler.c
OBJ     = $(SRC:.c=.o)
TARGET  = jappl2sb3

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lz

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
