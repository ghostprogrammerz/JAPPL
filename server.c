#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "parser.h"
#include "emitter.h"

#define DEFAULT_PORT 8765
#define MAX_REQUEST  (4 * 1024 * 1024)  /* 4 MB */
#define TMPDIR       "/tmp"

/* ── helpers ──────────────────────────────────────────────────────── */

static void send_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return;
        buf += n; len -= n;
    }
}

static void send_str(int fd, const char *s) {
    send_all(fd, s, strlen(s));
}

/* Send a complete HTTP response with CORS headers */
static void http_respond(int fd, int status, const char *content_type,
                         const char *body, size_t body_len) {
    const char *reason = (status == 200) ? "OK"
                       : (status == 400) ? "Bad Request"
                       : (status == 405) ? "Method Not Allowed"
                       : "Internal Server Error";
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason, content_type, body_len);
    send_str(fd, header);
    if (body && body_len > 0)
        send_all(fd, body, body_len);
}

/* Read file into heap buffer; returns NULL on failure. */
static char *read_file_binary(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

/* ── request reader ───────────────────────────────────────────────── */

typedef struct {
    char   method[8];
    char   path[256];
    size_t content_length;
    char  *body;        /* heap-allocated, null-terminated */
    size_t body_len;
} Request;

/* Read until \r\n\r\n, then read body per Content-Length.
   Returns 0 on success, -1 on error/overflow. */
static int read_request(int fd, Request *req) {
    memset(req, 0, sizeof(*req));

    /* Read header section into a growing buffer */
    size_t cap = 8192, hlen = 0;
    char *hbuf = malloc(cap);
    if (!hbuf) return -1;

    /* Scan for \r\n\r\n */
    int header_done = 0;
    size_t header_end = 0;
    while (!header_done) {
        if (hlen >= cap) {
            cap *= 2;
            if (cap > MAX_REQUEST) { free(hbuf); return -1; }
            hbuf = realloc(hbuf, cap);
            if (!hbuf) return -1;
        }
        ssize_t n = read(fd, hbuf + hlen, cap - hlen);
        if (n <= 0) { free(hbuf); return -1; }
        hlen += n;
        /* search for \r\n\r\n */
        for (size_t i = (hlen >= 4 ? hlen - n : 0); i + 3 < hlen; i++) {
            if (hbuf[i]=='\r' && hbuf[i+1]=='\n' &&
                hbuf[i+2]=='\r' && hbuf[i+3]=='\n') {
                header_end = i + 4;
                header_done = 1;
                break;
            }
        }
    }

    /* Parse request line */
    char *p = hbuf;
    char *sp1 = memchr(p, ' ', header_end);
    if (!sp1) { free(hbuf); return -1; }
    size_t mlen = sp1 - p;
    if (mlen >= sizeof(req->method)) { free(hbuf); return -1; }
    memcpy(req->method, p, mlen);
    req->method[mlen] = '\0';

    p = sp1 + 1;
    char *sp2 = memchr(p, ' ', header_end - (p - hbuf));
    if (!sp2) { free(hbuf); return -1; }
    size_t plen = sp2 - p;
    if (plen >= sizeof(req->path)) plen = sizeof(req->path) - 1;
    memcpy(req->path, p, plen);
    req->path[plen] = '\0';

    /* Find Content-Length header */
    req->content_length = 0;
    char *line = memchr(hbuf, '\n', header_end);
    while (line && (size_t)(line - hbuf) < header_end) {
        line++;
        if (strncasecmp(line, "content-length:", 15) == 0) {
            req->content_length = (size_t)atoll(line + 15);
            break;
        }
        line = memchr(line, '\n', header_end - (line - hbuf));
    }

    /* Assemble body: bytes already read past header + remainder from socket */
    size_t already = hlen - header_end;
    size_t total   = req->content_length;

    if (total > MAX_REQUEST) { free(hbuf); return -1; }

    req->body = malloc(total + 1);
    if (!req->body) { free(hbuf); return -1; }

    if (already > total) already = total;
    memcpy(req->body, hbuf + header_end, already);
    free(hbuf);

    size_t got = already;
    while (got < total) {
        ssize_t n = read(fd, req->body + got, total - got);
        if (n <= 0) { free(req->body); req->body = NULL; return -1; }
        got += n;
    }
    req->body[total] = '\0';
    req->body_len    = total;
    return 0;
}

/* ── compile handler ──────────────────────────────────────────────── */

static void handle_compile(int fd, const char *src, size_t src_len) {
    (void)src_len;

    /* Write .sb3 to a temp file */
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), TMPDIR "/jappl_%d.sb3", (int)getpid());

    Parser p;
    parser_init(&p, src);
    Program *prog = parser_parse(&p);

    if (p.errors > 0) {
        /* errors were printed to stderr; send a generic message back */
        const char *msg = "Compilation failed — check server stderr for details.\n";
        http_respond(fd, 400, "text/plain", msg, strlen(msg));
        return;
    }

    if (emit_sb3(prog, tmp_path) != 0) {
        const char *msg = "Emitter failed to write .sb3\n";
        http_respond(fd, 500, "text/plain", msg, strlen(msg));
        return;
    }

    size_t sb3_len = 0;
    char *sb3 = read_file_binary(tmp_path, &sb3_len);
    unlink(tmp_path);

    if (!sb3) {
        const char *msg = "Failed to read compiled .sb3\n";
        http_respond(fd, 500, "text/plain", msg, strlen(msg));
        return;
    }

    http_respond(fd, 200, "application/zip", sb3, sb3_len);
    free(sb3);
}

/* ── connection handler ───────────────────────────────────────────── */

static void handle_connection(int fd) {
    Request req;
    if (read_request(fd, &req) != 0) {
        close(fd);
        return;
    }

    /* CORS preflight */
    if (strcmp(req.method, "OPTIONS") == 0) {
        http_respond(fd, 200, "text/plain", "", 0);
        goto done;
    }

    if (strcmp(req.path, "/compile") == 0) {
        if (strcmp(req.method, "POST") != 0) {
            http_respond(fd, 405, "text/plain", "Method Not Allowed\n", 19);
            goto done;
        }
        if (!req.body || req.body_len == 0) {
            http_respond(fd, 400, "text/plain", "Empty body\n", 11);
            goto done;
        }
        handle_compile(fd, req.body, req.body_len);
        goto done;
    }

    {
        const char *msg = "Not Found\n";
        http_respond(fd, 404, "text/plain", msg, strlen(msg));
    }

done:
    if (req.body) free(req.body);
    close(fd);
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc >= 2) port = atoi(argv[1]);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(srv, 16) < 0) { perror("listen"); return 1; }

    printf("jappl2sb3 server listening on http://0.0.0.0:%d\n", port);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int fd = accept(srv, (struct sockaddr *)&client_addr, &client_len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        handle_connection(fd);
    }
}
