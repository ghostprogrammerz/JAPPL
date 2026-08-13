CC      = gcc
CFLAGS  = -Wall -Wextra -g -std=c11
IFLAGS  = -Icompiler -Idecompiler

COMPILER_SRC = compiler/lexer.c compiler/ast.c compiler/parser.c compiler/emitter.c
DECOMP_SRC   = decompiler/decompiler.c decompiler/decompiler_fix.c
COMMON_SRC   = $(COMPILER_SRC) $(DECOMP_SRC)
COMMON_OBJ   = $(COMMON_SRC:.c=.o)

TARGET  = jappl2sb3
SRV     = jappl2sb3-server

all: $(TARGET) $(SRV)

$(TARGET): main.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lz

$(SRV): runtime/server.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lz

%.o: %.c
	$(CC) $(CFLAGS) $(IFLAGS) -c $< -o $@

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
	rm -f main.o $(COMMON_OBJ) runtime/server.o $(TARGET) $(SRV)

.PHONY: all run kill clean
