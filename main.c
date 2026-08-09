#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "emitter.h"
#include "decompiler.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f); fclose(f);
    buf[sz] = '\0';
    return buf;
}

static char *replace_ext(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    int base_len = dot ? (int)(dot - path) : (int)strlen(path);
    char *out = malloc(base_len + strlen(ext) + 1);
    strncpy(out, path, base_len);
    strcpy(out + base_len, ext);
    return out;
}

static void usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  jappl2sb3 <file.jappl> [output.sb3]     compile\n"
        "  jappl2sb3 --decompile <file.sb3> [out.zip]     decompile to zip\n"
    );
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "--decompile") == 0) {
        if (argc < 3) { usage(); return 1; }
        const char *in  = argv[2];
        char *out = argc >= 4 ? strdup(argv[3]) : replace_ext(in, ".zip");
        int r = decompile_sb3(in, out);
        if (r == 0) printf("wrote %s\n", out);
        free(out);
        return r;
    }

    const char *in  = argv[1];
    char *out = argc >= 3 ? strdup(argv[2]) : replace_ext(in, ".sb3");

    char *src = read_file(in);
    Parser p; parser_init(&p, src);
    Program *prog = parser_parse(&p);

    if (p.errors > 0) {
        fprintf(stderr, "%d error(s) — aborting\n", p.errors);
        free(src); free(out); return 1;
    }
    if (emit_sb3(prog, out) != 0) {
        fprintf(stderr, "failed to write %s\n", out);
        free(src); free(out); return 1;
    }
    printf("wrote %s\n", out);
    free(src); free(out);
    return 0;
}
