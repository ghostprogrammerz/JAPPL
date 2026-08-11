#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "parser.h"
#include "emitter.h"

#define DEFAULT_PORT 8765
#define MAX_REQUEST (4 * 1024 * 1024)
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

static void send_str(int fd, const char *s) {
    send_all(fd, s, strlen(s));
}

static void http_respond(int fd, int status, const char *type,
                         const char *body, size_t len) {
    const char *reason = status == 200 ? "OK" :
                         status == 400 ? "Bad Request" :
                         status == 403 ? "Forbidden" :
                         status == 404 ? "Not Found" :
                         status == 405 ? "Method Not Allowed" :
                         "Internal Server Error";
    char header[768];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status, reason, type, len);
    send_str(fd, header);
    if (body && len) send_all(fd, body, len);
}

static char *read_file_binary(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size <= 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)size;
    return buf;
}

typedef struct {
    char method[8];
    char path[256];
    size_t content_length;
    char *body;
    size_t body_len;
} Request;

static int read_request(int fd, Request *r) {
    memset(r, 0, sizeof(*r));

    size_t cap = 8192;
    size_t hlen = 0;
    size_t header_end = 0;
    char *headers = malloc(cap);
    if (!headers) return -1;

    for (;;) {
        if (hlen >= cap) {
            cap *= 2;
            if (cap > MAX_REQUEST) { free(headers); return -1; }
            char *tmp = realloc(headers, cap);
            if (!tmp) { free(headers); return -1; }
            headers = tmp;
        }

        ssize_t n = read(fd, headers + hlen, cap - hlen);
        if (n <= 0) { free(headers); return -1; }
        hlen += (size_t)n;

        size_t start = hlen >= (size_t)n + 3 ? hlen - (size_t)n - 3 : 0;
        for (size_t i = start; i + 3 < hlen; i++) {
            if (headers[i] == '\r' && headers[i + 1] == '\n' &&
                headers[i + 2] == '\r' && headers[i + 3] == '\n') {
                header_end = i + 4;
                goto headers_done;
            }
        }
    }

headers_done:
    {
        char *p = headers;
        char *sp1 = memchr(p, ' ', header_end);
        if (!sp1) { free(headers); return -1; }
        size_t method_len = (size_t)(sp1 - p);
        if (method_len >= sizeof(r->method)) { free(headers); return -1; }
        memcpy(r->method, p, method_len);
        r->method[method_len] = '\0';

        p = sp1 + 1;
        char *sp2 = memchr(p, ' ', header_end - (size_t)(p - headers));
        if (!sp2) { free(headers); return -1; }
        size_t path_len = (size_t)(sp2 - p);
        if (path_len >= sizeof(r->path)) path_len = sizeof(r->path) - 1;
        memcpy(r->path, p, path_len);
        r->path[path_len] = '\0';
    }

    r->content_length = 0;
    char *line = memchr(headers, '\n', header_end);
    while (line) {
        line++;
        if (strncasecmp(line, "content-length:", 15) == 0) {
            r->content_length = (size_t)atoll(line + 15);
            break;
        }
        if ((size_t)(line - headers) >= header_end) break;
        line = memchr(line, '\n', header_end - (size_t)(line - headers));
    }

    if (r->content_length > MAX_REQUEST) {
        free(headers);
        return -1;
    }

    size_t already = hlen - header_end;
    size_t total = r->content_length;
    r->body = malloc(total + 1);
    if (!r->body) { free(headers); return -1; }
    if (already > total) already = total;
    if (already) memcpy(r->body, headers + header_end, already);
    free(headers);

    size_t got = already;
    while (got < total) {
        ssize_t n = read(fd, r->body + got, total - got);
        if (n <= 0) {
            free(r->body);
            r->body = NULL;
            return -1;
        }
        got += (size_t)n;
    }
    r->body[total] = '\0';
    r->body_len = total;
    return 0;
}

static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js")) return "application/javascript; charset=utf-8";
    if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
    return "application/octet-stream";
}

/*
 * The IDE contains the stage iframe and the normal compile functions.
 * Override only the runtime controls so the generated Packager HTML is
 * loaded from this server. No request is made to turbowarp.org.
 */
static const char *preview_controller =
    "<script>(function(){"
    "function frame(){return document.getElementById('stage-iframe');}"
    "function backend(){var e=document.getElementById('backend-url');return e?e.value.trim():location.origin;}"
    "window.loadProject=function(){"
        "var f=frame();if(!f)return;"
        "var p=document.getElementById('stage-placeholder');"
        "var l=document.getElementById('stage-label');"
        "if(p)p.style.display='none';"
        "f.style.display='block';"
        "if(l)l.textContent='Loading…';"
        "f.onload=function(){if(l)l.textContent='Running';if(typeof setStatus==='function')setStatus('running','ok');};"
        "f.onerror=function(){if(l)l.textContent='Preview failed';if(typeof setStatus==='function')setStatus('preview failed','err');};"
        "f.src=backend()+'/preview?ts='+Date.now();"
    "};"
    "window.greenFlag=function(){"
        "var f=frame();"
        "if(!f||f.style.display==='none'){if(typeof compileAndRun==='function')compileAndRun();return;}"
        "f.src=f.src.split('?')[0]+'?ts='+Date.now();"
    "};"
    "window.stopAll=function(){"
        "var f=frame();if(!f)return;"
        "f.src='about:blank';f.style.display='none';"
        "var p=document.getElementById('stage-placeholder');"
        "var l=document.getElementById('stage-label');"
        "if(p)p.style.display='flex';if(l)l.textContent='Stopped';"
        "if(typeof setStatus==='function')setStatus('stopped','');"
    "};"
    "window.fullscreen=function(){var f=frame();if(f&&f.requestFullscreen)f.requestFullscreen();};"
    "})();</script>";

static void handle_static(int fd, const char *url) {
    char path[4096];

    if (!strcmp(url, "/") || !strcmp(url, "/index.html")) {
        size_t len = 0;
        char *html = read_file_binary(g_ide_html, &len);
        if (!html) {
            http_respond(fd, 404, "text/plain", "Not Found\n", 10);
            return;
        }

        const char marker[] = "</body>";
        size_t marker_len = sizeof(marker) - 1;
        size_t off = len;
        while (off >= marker_len) {
            size_t pos = off - marker_len;
            if (!memcmp(html + pos, marker, marker_len)) {
                off = pos;
                break;
            }
            if (pos == 0) { off = len; break; }
            off = pos;
        }

        if (off == len) {
            http_respond(fd, 200, "text/html; charset=utf-8", html, len);
            free(html);
            return;
        }

        size_t controller_len = strlen(preview_controller);
        size_t out_len = len + controller_len;
        char *out = malloc(out_len);
        if (!out) {
            free(html);
            http_respond(fd, 500, "text/plain", "Out of memory\n", 14);
            return;
        }
        memcpy(out, html, off);
        memcpy(out + off, preview_controller, controller_len);
        memcpy(out + off + controller_len, html + off, len - off);
        http_respond(fd, 200, "text/html; charset=utf-8", out, out_len);
        free(out);
        free(html);
        return;
    }

    if (!strncmp(url, "/static/", 8) || !strncmp(url, "/chunks/", 8)) {
        const char *slash = strchr(url + 1, '/');
        const char *file = slash ? slash + 1 : NULL;
        const char *subdir = url[1] == 's' ? "static" : "chunks";
        if (!file || strstr(file, "..") || strchr(file, '/')) {
            http_respond(fd, 403, "text/plain", "Forbidden\n", 10);
            return;
        }
        snprintf(path, sizeof(path), "%s/%s/%s", g_static_dir, subdir, file);
    } else {
        http_respond(fd, 404, "text/plain", "Not Found\n", 10);
        return;
    }

    size_t len = 0;
    char *body = read_file_binary(path, &len);
    if (!body) {
        http_respond(fd, 404, "text/plain", "Not Found\n", 10);
        return;
    }
    http_respond(fd, 200, mime_for(path), body, len);
    free(body);
}

static int package_sb3(const char *sb3, char **out, size_t *out_len) {
    char html[256];
    char command[8192];
    snprintf(html, sizeof(html), TMPDIR "/jappl_%d_preview.html", (int)getpid());
    snprintf(command, sizeof(command),
             "node \"%s/package-runtime.js\" \"%s\" \"%s\"",
             g_ide_dir, sb3, html);

    int rc = system(command);
    if (rc != 0) {
        fprintf(stderr, "TurboWarp Packager failed. Run: cd ide && npm install\n");
        unlink(html);
        return -1;
    }

    *out = read_file_binary(html, out_len);
    unlink(html);
    return *out ? 0 : -1;
}

static void handle_compile(int fd, const char *src, size_t src_len) {
    (void)src_len;

    char tmp[128];
    snprintf(tmp, sizeof(tmp), TMPDIR "/jappl_%d.sb3", (int)getpid());

    Parser parser;
    parser_init(&parser, src);
    Program *program = parser_parse(&parser);
    if (parser.errors > 0) {
        http_respond(fd, 400, "text/plain",
                     "Compilation failed — check server stderr for details.\n", 56);
        return;
    }

    if (emit_sb3(program, tmp) != 0) {
        http_respond(fd, 500, "text/plain",
                     "Emitter failed to write .sb3\n", 31);
        return;
    }

    size_t len = 0;
    char *sb3 = read_file_binary(tmp, &len);
    if (!sb3) {
        unlink(tmp);
        http_respond(fd, 500, "text/plain",
                     "Failed to read compiled .sb3\n", 31);
        return;
    }

    char *preview = NULL;
    size_t preview_len = 0;
    if (package_sb3(tmp, &preview, &preview_len) == 0) {
        free(g_preview_buf);
        g_preview_buf = preview;
        g_preview_len = preview_len;
    } else {
        free(g_preview_buf);
        g_preview_buf = NULL;
        g_preview_len = 0;
    }

    unlink(tmp);
    free(g_sb3_buf);
    g_sb3_buf = sb3;
    g_sb3_len = len;

    http_respond(fd, 200, "application/zip", g_sb3_buf, g_sb3_len);
}

static void handle_connection(int fd) {
    Request r;
    if (read_request(fd, &r) != 0) {
        close(fd);
        return;
    }

    if (!strcmp(r.method, "OPTIONS")) {
        http_respond(fd, 200, "text/plain", "", 0);
        goto done;
    }

    if (!strcmp(r.path, "/") || !strcmp(r.path, "/index.html") ||
        !strncmp(r.path, "/static/", 8) || !strncmp(r.path, "/chunks/", 8)) {
        handle_static(fd, r.path);
        goto done;
    }

    if (!strncmp(r.path, "/preview", 8)) {
        if (g_preview_buf) {
            http_respond(fd, 200, "text/html; charset=utf-8",
                         g_preview_buf, g_preview_len);
        } else {
            const char *msg = "No packaged project yet. Compile first and run npm install in ide/.\n";
            http_respond(fd, 404, "text/plain", msg, strlen(msg));
        }
        goto done;
    }

    if (!strcmp(r.path, "/download")) {
        if (g_sb3_buf) {
            http_respond(fd, 200, "application/zip", g_sb3_buf, g_sb3_len);
        } else {
            const char *msg = "No project compiled yet\n";
            http_respond(fd, 404, "text/plain", msg, strlen(msg));
        }
        goto done;
    }

    if (!strcmp(r.path, "/compile")) {
        if (strcmp(r.method, "POST")) {
            http_respond(fd, 405, "text/plain", "Method Not Allowed\n", 19);
            goto done;
        }
        if (!r.body || !r.body_len) {
            http_respond(fd, 400, "text/plain", "Empty body\n", 11);
            goto done;
        }
        handle_compile(fd, r.body, r.body_len);
        goto done;
    }

    http_respond(fd, 404, "text/plain", "Not Found\n", 10);

done:
    free(r.body);
    close(fd);
}

int main(int argc, char **argv) {
    int port = argc >= 2 ? atoi(argv[1]) : DEFAULT_PORT;

    char exe[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) *slash = '\0';
        snprintf(g_ide_dir, sizeof(g_ide_dir), "%s/ide", exe);
        snprintf(g_static_dir, sizeof(g_static_dir), "%s/ide/static", exe);
        snprintf(g_ide_html, sizeof(g_ide_html), "%s/ide/index.html", exe);
    } else {
        snprintf(g_ide_dir, sizeof(g_ide_dir), "ide");
        snprintf(g_static_dir, sizeof(g_static_dir), "ide/static");
        snprintf(g_ide_html, sizeof(g_ide_html), "ide/index.html");
    }

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server, 16) < 0) {
        perror("listen");
        return 1;
    }

    printf("jappl2sb3 server listening on http://0.0.0.0:%d\n", port);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int fd = accept(server, (struct sockaddr *)&client, &client_len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        handle_connection(fd);
    }
}
