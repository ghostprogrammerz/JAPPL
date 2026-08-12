#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <ctype.h>

#include "../compiler/parser.h"
#include "../compiler/emitter.h"
#include "../decompiler/decompiler.h"

#define DEFAULT_PORT 8765
#define MAX_REQUEST (32 * 1024 * 1024)
#define TMPDIR "/tmp"

static char g_static_dir[4096];
static char g_ide_html[4096];
static char g_ide_dir[4096];
static char *g_sb3_buf;
static size_t g_sb3_len;
static char *g_preview_buf;
static size_t g_preview_len;

static void send_all(int fd, const char *buf, size_t len) {
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return;
        buf += n;
        len -= (size_t)n;
    }
}

