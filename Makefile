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

PORT ?= 8080

run: all
	@test -d ide/node_modules || (echo "Installing npm deps..." && cd ide && npm install)
	@kill $$(pgrep -x jappl2sb3-server) 2>/dev/null || true
	@sleep 0.1
	@./jappl2sb3-server $(PORT) &
	@sleep 0.3
	@echo "Server started → http://localhost:$(PORT)"
	@xdg-open "http://localhost:$(PORT)" 2>/dev/null || true

kill:
	@kill $$(pgrep -x jappl2sb3-server) 2>/dev/null && echo "Server stopped" || echo "Not running"

clean:
	rm -f $(OBJ) $(SRV_OBJ) $(TARGET) $(SRV)

.PHONY: all run kill clean
