#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
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
        "  jappl2sb3 <file.jappl> [output.sb3]          compile\n"
        "  jappl2sb3 <file.zip>   [output.sb3]          compile from decompiler zip\n"
        "  jappl2sb3 --decompile <file.sb3> [out.zip]   decompile to zip\n"
    );
}

/* ── minimal zip reader (mirrors decompiler's zip_read_file) ─────── */
static char *zip_extract(const char *path, const char *name, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(sz);
    fread(data, 1, sz, f); fclose(f);

    int pos = 0;
    while (pos + 30 < sz) {
        if (data[pos]!=0x50||data[pos+1]!=0x4b||data[pos+2]!=0x03||data[pos+3]!=0x04) break;
        int comp = data[pos+8] |(data[pos+9]<<8);
        int csz  = data[pos+18]|(data[pos+19]<<8)|(data[pos+20]<<16)|(data[pos+21]<<24);
        int usz  = data[pos+22]|(data[pos+23]<<8)|(data[pos+24]<<16)|(data[pos+25]<<24);
        int nlen = data[pos+26]|(data[pos+27]<<8);
        int elen = data[pos+28]|(data[pos+29]<<8);
        char fname[512] = {0};
        int fnlen = nlen < 511 ? nlen : 511;
        memcpy(fname, data+pos+30, fnlen);
        int data_start = pos+30+nlen+elen;

        if (strcmp(fname, name) == 0) {
            char *out = malloc(usz + 1);
            if (comp == 0) {
                memcpy(out, data+data_start, usz);
            } else if (comp == 8) {
                z_stream zs = {0};
                zs.next_in   = data+data_start;
                zs.avail_in  = csz;
                zs.next_out  = (Bytef*)out;
                zs.avail_out = usz;
                inflateInit2(&zs, -15);
                inflate(&zs, Z_FINISH);
                inflateEnd(&zs);
            } else { free(out); free(data); return NULL; }
            out[usz] = 0;
            if (out_len) *out_len = usz;
            free(data);
            return out;
        }
        pos = data_start + csz;
    }
    free(data);
    return NULL;
}

/* list all filenames in a zip (returns heap array of heap strings, sets *count) */
static char **zip_list_files(const char *path, int *count) {
    FILE *f = fopen(path, "rb");
    if (!f) { *count = 0; return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(sz);
    fread(data, 1, sz, f); fclose(f);

    char **names = NULL; int n = 0, cap = 0;
    int pos = 0;
    while (pos + 30 < sz) {
        if (data[pos]!=0x50||data[pos+1]!=0x4b||data[pos+2]!=0x03||data[pos+3]!=0x04) break;
        int csz  = data[pos+18]|(data[pos+19]<<8)|(data[pos+20]<<16)|(data[pos+21]<<24);
        int nlen = data[pos+26]|(data[pos+27]<<8);
        int elen = data[pos+28]|(data[pos+29]<<8);
        char fname[512] = {0};
        int fnlen = nlen < 511 ? nlen : 511;
        memcpy(fname, data+pos+30, fnlen);
        int data_start = pos+30+nlen+elen;

        if (n >= cap) { cap = cap ? cap*2 : 8; names = realloc(names, sizeof(char*)*cap); }
        names[n++] = strdup(fname);
        pos = data_start + csz;
    }
    free(data);
    *count = n;
    return names;
}

/* ── parse variables.csv → StateTable vars ───────────────────────── */
static void parse_csv(const char *csv, StateTable *st) {
    const char *p = csv;
    /* skip header line */
    while (*p && *p != '\n') p++;
    if (*p) p++;

    while (*p) {
        /* read name (unquoted, up to comma) */
        const char *name_start = p;
        while (*p && *p != ',' && *p != '\n') p++;
        if (!*p || *p == '\n') break;
        int name_len = p - name_start;
        char *name = malloc(name_len + 1);
        memcpy(name, name_start, name_len); name[name_len] = 0;
        p++; /* skip comma */

        /* read value (may be CSV-quoted) */
        char val_buf[4096] = {0}; int vl = 0;
        if (*p == '"') {
            p++;
            while (*p) {
                if (*p == '"' && *(p+1) == '"') { val_buf[vl++] = '"'; p += 2; }
                else if (*p == '"') { p++; break; }
                else val_buf[vl++] = *p++;
            }
        } else {
            while (*p && *p != '\n') val_buf[vl++] = *p++;
        }
        val_buf[vl] = 0;
        while (*p && *p != '\n') p++;
        if (*p) p++;

        /* add to table */

        st->vars = realloc(st->vars, sizeof(StateVar)*(st->var_count+1));
        st->vars[st->var_count].name  = name;
        st->vars[st->var_count].value = strdup(val_buf);
        st->var_count++;
    }
}

/* ── parse <listname>.txt → one StateList ────────────────────────── */
static void parse_list_txt(const char *txt, const char *listname, StateTable *st) {
    st->lists = realloc(st->lists, sizeof(StateList)*(st->list_count+1));
    StateList *ls = &st->lists[st->list_count++];
    ls->name  = strdup(listname);
    ls->items = NULL;
    ls->count = 0;

    const char *p = txt;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        int len = p - line_start;
        if (*p) p++;
        if (len == 0) continue;
        ls->items = realloc(ls->items, sizeof(char*)*(ls->count+1));
        ls->items[ls->count] = malloc(len+1);
        memcpy(ls->items[ls->count], line_start, len);
        ls->items[ls->count][len] = 0;
        ls->count++;
    }
}

/* ── compile a .zip produced by --decompile ──────────────────────── */
static int compile_zip(const char *zip_path, const char *sb3_path) {
    /* extract project.jappl */
    char *src = zip_extract(zip_path, "project.jappl", NULL);
    if (!src) { fprintf(stderr, "cannot find project.jappl in %s\n", zip_path); return -1; }

    /* build StateTable */
    StateTable st = {0};

    char *csv = zip_extract(zip_path, "variables.csv", NULL);
    if (csv) { parse_csv(csv, &st); free(csv); }

    /* find all *.txt files in the zip */
    int file_count = 0;
    char **files = zip_list_files(zip_path, &file_count);
    for (int i = 0; i < file_count; i++) {
        const char *fname = files[i];
        int flen = strlen(fname);
        if (flen > 4 && strcmp(fname + flen - 4, ".txt") == 0) {
            /* list name = filename without .txt */
            char listname[512];
            int lnlen = flen - 4 < 511 ? flen - 4 : 511;
            memcpy(listname, fname, lnlen); listname[lnlen] = 0;
            char *txt = zip_extract(zip_path, fname, NULL);
            if (txt) { parse_list_txt(txt, listname, &st); free(txt); }
        }
        free(files[i]);
    }
    free(files);

    /* parse and emit */
    Parser p; parser_init(&p, src);
    Program *prog = parser_parse(&p);
    int ret = 0;
    if (p.errors > 0) {
        fprintf(stderr, "%d error(s) — aborting\n", p.errors);
        ret = 1;
    } else if (emit_sb3_with_state(prog, sb3_path, &st) != 0) {
        fprintf(stderr, "failed to write %s\n", sb3_path);
        ret = 1;
    } else {
        printf("wrote %s\n", sb3_path);
    }
    free(src);
    state_table_free(&st);
    return ret;
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

    /* detect zip input */
    int in_len = strlen(in);
    int is_zip = (in_len > 4 && strcmp(in + in_len - 4, ".zip") == 0);

    int ret;
    if (is_zip) {
        ret = compile_zip(in, out);
    } else {
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
        free(src);
        ret = 0;
    }
    free(out);
    return ret;
}
