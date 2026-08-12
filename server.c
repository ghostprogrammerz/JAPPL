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

#include "parser.h"
#include "emitter.h"
#include "decompiler.h"

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
    send_all(fd, header, strlen(header));
    if (body && len) send_all(fd, body, len);
}

static char *read_file_binary(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    if (size && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    *out_len = (size_t)size;
    return buf;
}

typedef struct {
    char method[8];
    char path[256];
    char *body;
    size_t body_len;
    char raw_headers[8192];
} Request;

static int read_request(int fd, Request *r) {
    memset(r, 0, sizeof(*r));
    size_t cap = 8192, hlen = 0, header_end = 0;
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
        size_t old = hlen;
        hlen += (size_t)n;
        size_t start = old > 3 ? old - 3 : 0;
        for (size_t i = start; i + 3 < hlen; ++i) {
            if (headers[i] == '\r' && headers[i+1] == '\n' &&
                headers[i+2] == '\r' && headers[i+3] == '\n') {
                header_end = i + 4;
                goto headers_done;
            }
        }
    }

headers_done:
    {
        size_t copy = header_end < sizeof(r->raw_headers)-1 ? header_end : sizeof(r->raw_headers)-1;
        memcpy(r->raw_headers, headers, copy);
        r->raw_headers[copy] = '\0';
    }
    {
        char *sp1 = memchr(headers, ' ', header_end);
        if (!sp1) { free(headers); return -1; }
        size_t ml = (size_t)(sp1 - headers);
        if (ml >= sizeof(r->method)) { free(headers); return -1; }
        memcpy(r->method, headers, ml);
        r->method[ml] = '\0';

        char *p = sp1 + 1;
        char *sp2 = memchr(p, ' ', header_end - (size_t)(p - headers));
        if (!sp2) { free(headers); return -1; }
        size_t pl = (size_t)(sp2 - p);
        if (pl >= sizeof(r->path)) pl = sizeof(r->path) - 1;
        memcpy(r->path, p, pl);
        r->path[pl] = '\0';
    }

    size_t content_length = 0;
    char *line = memchr(headers, '\n', header_end);
    while (line) {
        ++line;
        if (strncasecmp(line, "content-length:", 15) == 0) {
            content_length = (size_t)atoll(line + 15);
            break;
        }
        if ((size_t)(line - headers) >= header_end) break;
        line = memchr(line, '\n', header_end - (size_t)(line - headers));
    }
    if (content_length > MAX_REQUEST) { free(headers); return -1; }

    size_t already = hlen - header_end;
    if (already > content_length) already = content_length;
    r->body = malloc(content_length + 1);
    if (!r->body) { free(headers); return -1; }
    if (already) memcpy(r->body, headers + header_end, already);
    free(headers);

    size_t got = already;
    while (got < content_length) {
        ssize_t n = read(fd, r->body + got, content_length - got);
        if (n <= 0) { free(r->body); r->body = NULL; return -1; }
        got += (size_t)n;
    }
    r->body[content_length] = '\0';
    r->body_len = content_length;
    return 0;
}

static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js")) return "application/javascript; charset=utf-8";
    if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".json")) return "application/json; charset=utf-8";
    return "application/octet-stream";
}

/*
 * Patch only the old preview functions in memory. This avoids injecting
 * JavaScript before the document and keeps ide/index.html valid HTML.
 * The packaged project itself is still generated locally by package-runtime.js.
 */
static char *build_ide_html(size_t *out_len) {
    size_t len = 0;
    char *html = read_file_binary(g_ide_html, &len);
    if (!html) return NULL;

    const char *start_marker = "async function loadProject(blob) {";
    const char *end_marker = "function updateStageSize()";
    char *start = strstr(html, start_marker);
    if (!start) {
        *out_len = len;
        return html;
    }
    char *end = strstr(start, end_marker);
    if (!end) {
        *out_len = len;
        return html;
    }

    static const char replacement[] =
        "async function loadProject(blob) {\n"
        "  setStatus('loading…', '');\n"
        "  document.getElementById('stage-label').textContent = 'Loading…';\n"
        "  document.getElementById('stage-placeholder').style.display = 'none';\n"
        "  const iframe = document.getElementById('stage-iframe');\n"
        "  iframe.style.display = 'block';\n"
        "  const backendUrl = document.getElementById('backend-url').value.trim() || location.origin;\n"
        "  iframe.onload = () => {\n"
        "    setStatus('running', 'ok');\n"
        "    document.getElementById('stage-label').textContent = 'Running';\n"
        "  };\n"
        "  iframe.onerror = () => {\n"
        "    setStatus('preview failed', 'err');\n"
        "    document.getElementById('stage-label').textContent = 'Preview failed';\n"
        "  };\n"
        "  iframe.src = backendUrl + '/preview?ts=' + Date.now();\n"
        "}\n\n"
        "function greenFlag() {\n"
        "  const iframe = document.getElementById('stage-iframe');\n"
        "  if (!iframe || iframe.style.display === 'none') { compileAndRun(); return; }\n"
        "  iframe.src = iframe.src.split('?')[0] + '?ts=' + Date.now();\n"
        "}\n\n"
        "function stopAll() {\n"
        "  const iframe = document.getElementById('stage-iframe');\n"
        "  if (!iframe) return;\n"
        "  iframe.src = 'about:blank';\n"
        "  iframe.style.display = 'none';\n"
        "  document.getElementById('stage-placeholder').style.display = 'flex';\n"
        "  document.getElementById('stage-label').textContent = 'Stopped';\n"
        "  setStatus('stopped');\n"
        "}\n\n";

    size_t old_len = (size_t)(end - start);
    size_t replacement_len = sizeof(replacement) - 1;
    size_t new_len = len - old_len + replacement_len;
    char *out = malloc(new_len + 1);
    if (!out) { free(html); return NULL; }

    size_t prefix = (size_t)(start - html);
    memcpy(out, html, prefix);
    memcpy(out + prefix, replacement, replacement_len);
    memcpy(out + prefix + replacement_len, end, len - (size_t)(end - html));
    out[new_len] = '\0';
    free(html);
    *out_len = new_len;
    return out;
}

static void handle_static(int fd, const char *url) {
    char path[4096];

    if (!strcmp(url, "/") || !strcmp(url, "/index.html")) {
        size_t len = 0;
        char *body = build_ide_html(&len);
        if (!body) {
            http_respond(fd, 404, "text/plain", "Not Found\n", 10);
            return;
        }
        http_respond(fd, 200, "text/html; charset=utf-8", body, len);
        free(body);
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
    } else if (!strncmp(url, "/docs/", 6) || !strcmp(url, "/docs")) {
        const char *file = url + 6; /* may be empty string for /docs/ */
        if (strstr(file, "..")) { http_respond(fd, 403, "text/plain", "Forbidden\n", 10); return; }
        if (!*file || !strcmp(file, "index.html")) file = "index.html";
        snprintf(path, sizeof(path), "%s/docs/%s", g_ide_dir, file);
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
    /* Optional IDE costume metadata follows this delimiter. It is deliberately
       kept outside JAPPL source so the normal parser remains unchanged. */
    static const char marker[] = "\n--JAPPL-COSTUMES--\n";
    const char *meta = strstr(src, marker);
    size_t source_len = meta ? (size_t)(meta - src) : src_len;
    char *source = malloc(source_len + 1);
    if (!source) {
        http_respond(fd, 500, "text/plain", "Out of memory\n", 14);
        return;
    }
    memcpy(source, src, source_len);
    source[source_len] = '\0';

    char meta_path[128] = {0};
    if (meta) {
        snprintf(meta_path, sizeof(meta_path), TMPDIR "/jappl_%d_costumes.json", (int)getpid());
        FILE *mf = fopen(meta_path, "wb");
        size_t meta_len = src_len - source_len - strlen(marker);
        if (!mf || fwrite(meta + strlen(marker), 1, meta_len, mf) != meta_len) {
            if (mf) fclose(mf);
            free(source);
            unlink(meta_path);
            http_respond(fd, 400, "text/plain", "Invalid costume metadata\n", 26);
            return;
        }
        fclose(mf);
    }

    char tmp[128];
    snprintf(tmp, sizeof(tmp), TMPDIR "/jappl_%d.sb3", (int)getpid());

    Parser parser;
    parser_init(&parser, source);
    Program *program = parser_parse(&parser);
    if (parser.errors > 0) {
        free(source);
        unlink(meta_path);
        http_respond(fd, 400, "text/plain",
                     "Compilation failed — check server stderr for details.\n", 56);
        return;
    }

    if (emit_sb3(program, tmp) != 0) {
        free(source);
        unlink(meta_path);
        http_respond(fd, 500, "text/plain", "Emitter failed to write .sb3\n", 31);
        return;
    }

    if (meta_path[0]) {
        char command[1024];
        snprintf(command, sizeof(command),
                 "node \"%s/patch-sb3.js\" \"%s\" \"%s\"",
                 g_ide_dir, tmp, meta_path);
        if (system(command) != 0) {
            free(source);
            unlink(meta_path);
            unlink(tmp);
            http_respond(fd, 400, "text/plain", "Failed to apply costume assets\n", 34);
            return;
        }
        unlink(meta_path);
    }
    free(source);

    size_t len = 0;
    char *sb3 = read_file_binary(tmp, &len);
    if (!sb3) {
        unlink(tmp);
        http_respond(fd, 500, "text/plain", "Failed to read compiled .sb3\n", 31);
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
    if (read_request(fd, &r) != 0) { close(fd); return; }

    if (!strcmp(r.method, "OPTIONS")) {
        http_respond(fd, 200, "text/plain", "", 0);
        goto done;
    }

    if (!strcmp(r.path, "/") || !strcmp(r.path, "/index.html") ||
        !strncmp(r.path, "/static/", 8) || !strncmp(r.path, "/chunks/", 8) ||
        !strncmp(r.path, "/docs", 5)) {
        handle_static(fd, r.path);
        goto done;
    }

    if (!strncmp(r.path, "/preview", 8)) {
        if (g_preview_buf) {
            http_respond(fd, 200, "text/html; charset=utf-8", g_preview_buf, g_preview_len);
        } else {
            const char *msg = "No packaged project yet. Compile first and run npm install in ide/.\n";
            http_respond(fd, 404, "text/plain", msg, strlen(msg));
        }
        goto done;
    }

    if (!strcmp(r.path, "/download")) {
        if (g_sb3_buf) http_respond(fd, 200, "application/zip", g_sb3_buf, g_sb3_len);
        else {
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

    if (!strcmp(r.path, "/decompile")) {
        if (strcmp(r.method, "POST")) {
            http_respond(fd, 405, "text/plain", "Method Not Allowed\n", 19);
            goto done;
        }
        if (!r.body || !r.body_len) {
            http_respond(fd, 400, "text/plain", "Empty body\n", 11);
            goto done;
        }

        /* derive a safe stem from X-Filename header, fallback to "project" */
        char stem[128] = "project";
        char *xfn = strstr(r.raw_headers, "x-filename:");
        if (!xfn) xfn = strcasestr(r.raw_headers, "x-filename:");
        if (xfn) {
            xfn += 11;
            while (*xfn == ' ') xfn++;
            char *end = strpbrk(xfn, "\r\n");
            size_t len = end ? (size_t)(end - xfn) : strlen(xfn);
            if (len > 64) len = 64;
            char raw[128] = {0};
            memcpy(raw, xfn, len);
            /* strip .sb3 extension */
            char *dot = strrchr(raw, '.');
            if (dot) *dot = '\0';
            /* sanitize: keep only alnum, dash, underscore */
            int si = 0;
            for (char *p = raw; *p && si < 120; p++) {
                if (isalnum((unsigned char)*p) || *p == '-' || *p == '_')
                    stem[si++] = *p;
                else if (*p == ' ')
                    stem[si++] = '_';
            }
            stem[si] = '\0';
            if (si == 0) strcpy(stem, "project");
        }

        /* ensure cache dir exists */
        char cache_dir[512];
        snprintf(cache_dir, sizeof(cache_dir), "%s/cache", g_ide_dir[0] ? g_ide_dir : ".");
        mkdir(cache_dir, 0755);

        char sb3_tmp[512], zip_out[512];
        snprintf(sb3_tmp, sizeof(sb3_tmp), TMPDIR "/jappl_%d_in.sb3", (int)getpid());
        snprintf(zip_out, sizeof(zip_out), "%s/%s.zip", cache_dir, stem);

        FILE *sf = fopen(sb3_tmp, "wb");
        if (!sf) { http_respond(fd, 500, "text/plain", "Cannot write temp file\n", 23); goto done; }
        fwrite(r.body, 1, r.body_len, sf);
        fclose(sf);

        if (decompile_sb3(sb3_tmp, zip_out) != 0) {
            unlink(sb3_tmp);
            http_respond(fd, 500, "text/plain", "Decompile failed\n", 17);
            goto done;
        }
        unlink(sb3_tmp);

        size_t zlen = 0;
        char *zbuf = read_file_binary(zip_out, &zlen);
        if (!zbuf) { http_respond(fd, 500, "text/plain", "Cannot read output zip\n", 23); goto done; }
        http_respond(fd, 200, "application/zip", zbuf, zlen);
        free(zbuf);
        goto done;
    }

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
        perror("bind"); return 1;
    }
    if (listen(server, 16) < 0) { perror("listen"); return 1; }

    printf("jappl2sb3 server listening on http://localhost:%d\n", port);
    fflush(stdout);

    /* open browser unless NO_BROWSER=1 */
    if (!getenv("NO_BROWSER")) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd),
                 "xdg-open 'http://localhost:%d' >/dev/null 2>&1 &", port);
        system(cmd);
    }

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
