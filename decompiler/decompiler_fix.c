#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <zlib.h>
#include "decompiler.h"

extern int decompile_sb3(const char *sb3_path, const char *out_zip_path);

typedef struct {
    char *name;
    unsigned char *data;
    size_t len;
} FixZipEntry;

static void fix_free_entries(FixZipEntry *e, int n) {
    for (int i = 0; i < n; ++i) { free(e[i].name); free(e[i].data); }
    free(e);
}

static int fix_read_zip(const char *path, FixZipEntry **out_entries, int *out_count) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    unsigned char *all = malloc((size_t)sz);
    if (!all) { fclose(f); return -1; }
    if (sz && fread(all, 1, (size_t)sz, f) != (size_t)sz) {
        free(all); fclose(f); return -1;
    }
    fclose(f);

    FixZipEntry *entries = NULL;
    int count = 0, cap = 0;
    size_t pos = 0;
    while (pos + 30 <= (size_t)sz) {
        if (all[pos] != 0x50 || all[pos+1] != 0x4b ||
            all[pos+2] != 0x03 || all[pos+3] != 0x04) break;
        unsigned comp = all[pos+8] | (all[pos+9] << 8);
        uint32_t csz = (uint32_t)all[pos+18] | ((uint32_t)all[pos+19] << 8) |
                       ((uint32_t)all[pos+20] << 16) | ((uint32_t)all[pos+21] << 24);
        uint32_t usz = (uint32_t)all[pos+22] | ((uint32_t)all[pos+23] << 8) |
                       ((uint32_t)all[pos+24] << 16) | ((uint32_t)all[pos+25] << 24);
        unsigned nlen = all[pos+26] | (all[pos+27] << 8);
        unsigned elen = all[pos+28] | (all[pos+29] << 8);
        size_t data_start = pos + 30u + nlen + elen;
        if (data_start > (size_t)sz || data_start + csz > (size_t)sz) break;

        char *name = malloc((size_t)nlen + 1);
        if (!name) { free(all); fix_free_entries(entries, count); return -1; }
        memcpy(name, all + pos + 30, nlen);
        name[nlen] = 0;

        unsigned char *data = malloc((size_t)usz + 1);
        if (!data) {
            free(name); free(all); fix_free_entries(entries, count); return -1;
        }
        if (comp == 0) {
            if (usz > csz) { free(name); free(data); free(all); fix_free_entries(entries, count); return -1; }
            memcpy(data, all + data_start, usz);
        } else if (comp == 8) {
            z_stream zs = {0};
            zs.next_in = all + data_start;
            zs.avail_in = csz;
            zs.next_out = data;
            zs.avail_out = usz;
            if (inflateInit2(&zs, -15) != Z_OK || inflate(&zs, Z_FINISH) != Z_STREAM_END) {
                inflateEnd(&zs);
                free(name); free(data); free(all); fix_free_entries(entries, count); return -1;
            }
            inflateEnd(&zs);
        } else {
            free(name); free(data); free(all); fix_free_entries(entries, count); return -1;
        }
        data[usz] = 0;

        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            FixZipEntry *tmp = realloc(entries, sizeof(*entries) * (size_t)cap);
            if (!tmp) {
                free(name); free(data); free(all); fix_free_entries(entries, count); return -1;
            }
            entries = tmp;
        }
        entries[count].name = name;
        entries[count].data = data;
        entries[count].len = usz;
        count++;
        pos = data_start + csz;
    }
    free(all);
    *out_entries = entries;
    *out_count = count;
    return 0;
}

static unsigned fix_crc32(const unsigned char *data, size_t len) {
    return (unsigned)crc32(0L, data, (uInt)len);
}
static void fix_u16(FILE *f, unsigned v) {
    fputc((int)(v & 255), f); fputc((int)((v >> 8) & 255), f);
}
static void fix_u32(FILE *f, uint32_t v) {
    fputc((int)(v & 255), f); fputc((int)((v >> 8) & 255), f);
    fputc((int)((v >> 16) & 255), f); fputc((int)((v >> 24) & 255), f);
}

static int fix_write_zip(const char *path, FixZipEntry *entries, int count) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.jappl-fix-%ld.tmp", path, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    uint32_t *offsets = calloc((size_t)count, sizeof(*offsets));
    uint32_t *crcs = calloc((size_t)count, sizeof(*crcs));
    if (!offsets || !crcs) { free(offsets); free(crcs); fclose(f); remove(tmp); return -1; }

    long pos = 0;
    for (int i = 0; i < count; ++i) {
        size_t nlen = strlen(entries[i].name);
        if (nlen > 65535 || entries[i].len > 0xffffffffu || pos > 0xffffffffL) {
            free(offsets); free(crcs); fclose(f); remove(tmp); return -1;
        }
        offsets[i] = (uint32_t)pos;
        crcs[i] = fix_crc32(entries[i].data, entries[i].len);
        fix_u32(f, 0x04034b50u);
        fix_u16(f, 20); fix_u16(f, 0); fix_u16(f, 0); fix_u16(f, 0); fix_u16(f, 0);
        fix_u32(f, crcs[i]);
        fix_u32(f, (uint32_t)entries[i].len); fix_u32(f, (uint32_t)entries[i].len);
        fix_u16(f, (unsigned)nlen); fix_u16(f, 0);
        fwrite(entries[i].name, 1, nlen, f);
        if (entries[i].len) fwrite(entries[i].data, 1, entries[i].len, f);
        pos = ftell(f);
        if (pos < 0) { free(offsets); free(crcs); fclose(f); remove(tmp); return -1; }
    }

    uint32_t cd_start = (uint32_t)ftell(f);
    for (int i = 0; i < count; ++i) {
        size_t nlen = strlen(entries[i].name);
        fix_u32(f, 0x02014b50u);
        fix_u16(f, 20); fix_u16(f, 20); fix_u16(f, 0); fix_u16(f, 0); fix_u16(f, 0); fix_u16(f, 0);
        fix_u32(f, crcs[i]);
        fix_u32(f, (uint32_t)entries[i].len); fix_u32(f, (uint32_t)entries[i].len);
        fix_u16(f, (unsigned)nlen); fix_u16(f, 0); fix_u16(f, 0); fix_u16(f, 0); fix_u16(f, 0);
        fix_u32(f, 0); fix_u32(f, offsets[i]);
        fwrite(entries[i].name, 1, nlen, f);
    }
    long end = ftell(f);
    if (end < 0) { free(offsets); free(crcs); fclose(f); remove(tmp); return -1; }
    uint32_t cd_size = (uint32_t)end - cd_start;
    fix_u32(f, 0x06054b50u);
    fix_u16(f, 0); fix_u16(f, 0); fix_u16(f, (unsigned)count); fix_u16(f, (unsigned)count);
    fix_u32(f, cd_size); fix_u32(f, cd_start); fix_u16(f, 0);
    if (fclose(f) != 0) { free(offsets); free(crcs); remove(tmp); return -1; }
    if (rename(tmp, path) != 0) { remove(tmp); free(offsets); free(crcs); return -1; }
    free(offsets); free(crcs);
    return 0;
}

static char *fix_trim_dup(const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\t')) { ++s; --n; }
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) --n;
    char *r = malloc(n + 1); if (!r) return NULL;
    memcpy(r, s, n); r[n] = 0; return r;
}
static int fix_outer_parens(const char *s) {
    size_t n = strlen(s); if (n < 2 || s[0] != '(' || s[n-1] != ')') return 0;
    int d = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '(') ++d;
        else if (s[i] == ')') { --d; if (d == 0 && i != n-1) return 0; if (d < 0) return 0; }
    }
    return d == 0;
}
static char *fix_wrap(const char *s) {
    char *t = fix_trim_dup(s, strlen(s)); if (!t) return NULL;
    if (!*t || fix_outer_parens(t)) return t;
    size_t n = strlen(t); char *r = malloc(n + 3); if (!r) { free(t); return NULL; }
    r[0] = '('; memcpy(r + 1, t, n); r[n + 1] = ')'; r[n + 2] = 0; free(t); return r;
}
static int fix_take_balanced(const char *s, size_t *used) {
    if (*s != '(') return 0; int d = 0;
    for (size_t i = 0; s[i]; ++i) { if (s[i] == '(') ++d; else if (s[i] == ')') { --d; if (d == 0) { *used = i + 1; return 1; } } }
    return 0;
}
static char *fix_take_expr(const char *s, size_t *used) {
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) { *used = 0; return strdup(""); }
    if (*s == '(') { size_t k = 0; if (fix_take_balanced(s, &k)) { *used = k; return fix_trim_dup(s, k); } }
    if (!strncmp(s, "variable (", 10)) { size_t k = 0; if (fix_take_balanced(s + 9, &k)) { *used = 9 + k; return fix_trim_dup(s, *used); } }
    if (!strncmp(s, "key (", 5)) { size_t k = 0; if (fix_take_balanced(s + 4, &k)) { k += 4; if (!strncmp(s + k, " pressed", 8)) k += 8; *used = k; return fix_trim_dup(s, *used); } }
    if (!strncmp(s, "touching (", 10)) { size_t k = 0; if (fix_take_balanced(s + 9, &k)) { *used = 9 + k; return fix_trim_dup(s, *used); } }
    if (!strncmp(s, "distance to (", 13)) { size_t k = 0; if (fix_take_balanced(s + 12, &k)) { *used = 12 + k; return fix_trim_dup(s, *used); } }
    if (!strncmp(s, "item (", 6) || !strncmp(s, "item num (", 10)) {
        size_t off = !strncmp(s, "item num (", 10) ? 9 : 5, k = 0;
        if (fix_take_balanced(s + off, &k)) { size_t p = off + k; if (!strncmp(s + p, " of list (", 10)) { size_t k2 = 0; if (fix_take_balanced(s + p + 9, &k2)) p += 9 + k2; } *used = p; return fix_trim_dup(s, p); }
    }
    if (!strncmp(s, "length of list (", 16)) { size_t k = 0; if (fix_take_balanced(s + 15, &k)) { *used = 15 + k; return fix_trim_dup(s, *used); } }
    if (!strncmp(s, "list (", 6)) { size_t k = 0; if (fix_take_balanced(s + 5, &k)) { size_t p = 5 + k; if (!strncmp(s + p, " contains (", 11)) { size_t k2 = 0; if (fix_take_balanced(s + p + 10, &k2)) p += 10 + k2; } *used = p; return fix_trim_dup(s, p); } }
    const char *simple[] = {"x position","y position","direction","size","volume","answer","timer","loudness","username","online","days since 2000","costume number","costume name","backdrop number","backdrop name","mouse x","mouse y",NULL};
    for (int i = 0; simple[i]; ++i) { size_t m = strlen(simple[i]); if (!strncmp(s, simple[i], m) && (!s[m] || s[m] == ' ' || s[m] == '\t' || s[m] == '\r')) { *used = m; return fix_trim_dup(s, m); } }
    if (!strncmp(s, "current ", 8)) { const char *p = s + 8; const char *names[] = {"day of week","year","month","date","hour","minute","second",NULL}; for (int i=0; names[i]; ++i) { size_t m = strlen(names[i]); if (!strncmp(p, names[i], m) && (!p[m] || p[m] == ' ' || p[m] == '\r')) { *used = 8 + m; return fix_trim_dup(s, *used); } } }
    if (!strncmp(s, "pick random ", 12)) { size_t k1 = 0; if (fix_take_balanced(s + 11, &k1)) { size_t p = 11 + k1; if (!strncmp(s + p, " to ", 4)) { size_t k2 = 0; if (fix_take_balanced(s + p + 4, &k2)) p += 4 + k2; } *used = p; return fix_trim_dup(s, p); } }
    if (!strncmp(s, "join(", 5) || !strncmp(s, "letter (", 8) || !strncmp(s, "round (", 7) || !strncmp(s, "exp (", 5) || !strncmp(s, "exp10 (", 7) || !strncmp(s, "ln (", 4) || !strncmp(s, "log (", 5) || !strncmp(s, "sqrt (", 6) || !strncmp(s, "abs (", 5) || !strncmp(s, "floor (", 7) || !strncmp(s, "ceiling (", 9) || !strncmp(s, "sin (", 5) || !strncmp(s, "cos (", 5) || !strncmp(s, "tan (", 5) || !strncmp(s, "not (", 5)) {
        const char *lp = strchr(s, '('); size_t k = 0; if (lp && fix_take_balanced(lp, &k)) { *used = (size_t)(lp - s) + k; return fix_trim_dup(s, *used); }
    }
    size_t i = 0; if (s[i] == '-') ++i; while (s[i] >= '0' && s[i] <= '9') ++i; if (s[i] == '.') { ++i; while (s[i] >= '0' && s[i] <= '9') ++i; }
    if (i > 0 && (!s[i] || s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) { *used = i; return fix_trim_dup(s, i); }
    i = 0; while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != '\r') ++i; *used = i; return fix_trim_dup(s, i);
}
static char *fix_wrap_exprs(const char *s, int count) {
    char *rest = fix_trim_dup(s, strlen(s)); if (!rest) return NULL; size_t cap = strlen(rest) + 64; char *out = malloc(cap); if (!out) { free(rest); return NULL; } out[0] = 0;
    for (int i = 0; i < count; ++i) { size_t used = 0; char *expr = fix_take_expr(rest, &used); if (!expr || !used) { free(expr); free(rest); free(out); return NULL; } char *wrapped = fix_wrap(expr); if (!wrapped) { free(expr); free(rest); free(out); return NULL; } size_t need = strlen(out) + strlen(wrapped) + 3; if (need >= cap) { while (need >= cap) cap *= 2; out = realloc(out, cap); } if (i) strcat(out, " "); strcat(out, wrapped); free(expr); free(wrapped); memmove(rest, rest + used, strlen(rest + used) + 1); while (*rest == ' ' || *rest == '\t') memmove(rest, rest + 1, strlen(rest)); }
    free(rest); return out;
}
static char *fix_wrap_all_exprs(const char *s) {
    char *rest = fix_trim_dup(s, strlen(s)); if (!rest) return NULL; size_t cap = strlen(rest) + 64; char *out = malloc(cap); if (!out) { free(rest); return NULL; } out[0] = 0; int first = 1;
    while (*rest) { size_t used = 0; char *expr = fix_take_expr(rest, &used); if (!expr || !used) { free(expr); free(rest); free(out); return NULL; } char *wrapped = fix_wrap(expr); if (!wrapped) { free(expr); free(rest); free(out); return NULL; } size_t need = strlen(out) + strlen(wrapped) + 3; if (need >= cap) { while (need >= cap) cap *= 2; out = realloc(out, cap); } if (!first) strcat(out, " "); strcat(out, wrapped); first = 0; free(expr); free(wrapped); memmove(rest, rest + used, strlen(rest + used) + 1); while (*rest == ' ' || *rest == '\t') memmove(rest, rest + 1, strlen(rest)); }
    free(rest); return out;
}
static char *fix_replace_one(const char *prefix, const char *expr, const char *suffix) { char *w = fix_wrap(expr); if (!w) return NULL; size_t n = strlen(prefix) + strlen(w) + strlen(suffix) + 1; char *r = malloc(n); if (!r) { free(w); return NULL; } snprintf(r, n, "%s%s%s", prefix, w, suffix); free(w); return r; }

typedef struct { char **name; int count; int cap; } FixProcNames;
static void fix_add_proc(FixProcNames *p, const char *name) { if (!name || !*name) return; for (int i=0;i<p->count;i++) if (!strcmp(p->name[i], name)) return; if (p->count >= p->cap) { p->cap = p->cap ? p->cap*2 : 8; p->name = realloc(p->name, sizeof(char*) * (size_t)p->cap); } p->name[p->count++] = strdup(name); }
static void fix_free_procs(FixProcNames *p) { for (int i=0;i<p->count;i++) free(p->name[i]); free(p->name); }
static int fix_starts_with(const char *line, const char *prefix) { return strncmp(line, prefix, strlen(prefix)) == 0; }

static char *fix_line(const char *line, FixProcNames *procs) {
    size_t len = strlen(line), ind = 0; while (ind < len && (line[ind] == ' ' || line[ind] == '\t')) ++ind;
    const char *body = line + ind; char *core = fix_trim_dup(body, strlen(body)); if (!core) return NULL;
    if (!*core || core[0]=='/' || !strcmp(core,"{") || !strcmp(core,"}") || !strcmp(core,"else")) goto unchanged;
    if (fix_starts_with(core,"var (") || fix_starts_with(core,"list (") || fix_starts_with(core,"sprite (") || !strcmp(core,"stage") || fix_starts_with(core,"custom block (") || fix_starts_with(core,"define (") || fix_starts_with(core,"when (")) goto unchanged;

    char *out = NULL;
    if (fix_starts_with(core,"move ") && strstr(core," steps")) { char *tail=strdup(core+5); char *p=strstr(tail," steps"); *p=0; out=fix_replace_one("move ",tail," steps"); free(tail); }
    else if ((fix_starts_with(core,"turn right ") || fix_starts_with(core,"turn left ")) && strstr(core," degrees")) { const char *pre=fix_starts_with(core,"turn right ")?"turn right ":"turn left "; char *tail=strdup(core+strlen(pre)); char *p=strstr(tail," degrees"); *p=0; out=fix_replace_one(pre,tail," degrees"); free(tail); }
    else if (!strncmp(core,"go to ",6) && !strstr(core," layer")) { out=fix_wrap_exprs(core+6,2); if(out){char *tmp=malloc(strlen(out)+7);sprintf(tmp,"go to %s",out);free(out);out=tmp;} }
    else if (!strncmp(core,"glide ",6) && strstr(core," secs to ")) { char *tail=strdup(core+6); char *sep=strstr(tail," secs to "); *sep=0; char *a=fix_wrap(tail), *two=fix_wrap_exprs(sep+9,2); if(a&&two){out=malloc(strlen(a)+strlen(two)+12);sprintf(out,"glide %s secs to %s",a,two);} free(a);free(two);free(tail); }
    else if (!strncmp(core,"say ",4) || !strncmp(core,"think ",6)) { const char *pre=!strncmp(core,"say ",4)?"say ":"think "; const char *tail0=core+strlen(pre); const char *forp=strstr(tail0," for "); if(forp&&strstr(forp+5," seconds")){char *a=fix_trim_dup(tail0,(size_t)(forp-tail0)),*b=strdup(forp+5);char *sec=strstr(b," seconds");if(sec)*sec=0;char *wa=fix_wrap(a),*wb=fix_wrap(b);if(wa&&wb){out=malloc(strlen(pre)+strlen(wa)+strlen(wb)+16);sprintf(out,"%s%s for %s seconds",pre,wa,wb);}free(a);free(b);free(wa);free(wb);}else{char*w=fix_wrap(tail0);if(w){out=malloc(strlen(pre)+strlen(w)+1);sprintf(out,"%s%s",pre,w);free(w);}} }
    else if (!strncmp(core,"set variable (",14) || !strncmp(core,"change variable (",17)) { const char *kw=!strncmp(core,"set variable (",14)?"set variable (":"change variable ("; const char *close=strchr(core+strlen(kw),')'); const char *join=close?(strstr(close," to ")?strstr(close," to "):strstr(close," by ")):NULL; if(close&&join){size_t prefix_len=(size_t)(join-core);char*prefix=fix_trim_dup(core,prefix_len);char*w=fix_wrap(join+4);if(prefix&&w){out=malloc(strlen(prefix)+1+strlen(w)+1);sprintf(out,"%s %s",prefix,w);}free(prefix);free(w);} }
    else if (!strncmp(core,"point in direction ",20) || !strncmp(core,"wait until ",11) || !strncmp(core,"wait ",5) || !strncmp(core,"ask ",4)) { const char *pre=!strncmp(core,"point in direction ",20)?"point in direction ":(!strncmp(core,"wait until ",11)?"wait until ":(!strncmp(core,"wait ",5)?"wait ":"ask ")); char*w=fix_wrap(core+strlen(pre));if(w){out=malloc(strlen(pre)+strlen(w)+1);sprintf(out,"%s%s",pre,w);free(w);} }
    else if (!strncmp(core,"set x to ",9) || !strncmp(core,"set y to ",9) || !strncmp(core,"set size to ",12) || !strncmp(core,"set volume to ",14) || !strncmp(core,"change x by ",12) || !strncmp(core,"change y by ",12) || !strncmp(core,"change size by ",15) || !strncmp(core,"change volume by ",17) || !strncmp(core,"change color effect by ",23) || !strncmp(core,"change fisheye effect by ",25) || !strncmp(core,"change whirl effect by ",23) || !strncmp(core,"change pixelate effect by ",26) || !strncmp(core,"change mosaic effect by ",24) || !strncmp(core,"change brightness effect by ",28) || !strncmp(core,"change ghost effect by ",22) || !strncmp(core,"change pitch effect by ",23) || !strncmp(core,"change pan effect by ",21) || !strncmp(core,"set color effect to ",21) || !strncmp(core,"set fisheye effect to ",23) || !strncmp(core,"set whirl effect to ",21) || !strncmp(core,"set pixelate effect to ",24) || !strncmp(core,"set mosaic effect to ",22) || !strncmp(core,"set brightness effect to ",26) || !strncmp(core,"set ghost effect to ",20) || !strncmp(core,"set pitch effect to ",21) || !strncmp(core,"set pan effect to ",19) || !strncmp(core,"set drag mode to ",17) || !strncmp(core,"set rotation style to ",22)) {
        const char *arr[]={"set x to ","set y to ","set size to ","set volume to ","change x by ","change y by ","change size by ","change volume by ","change color effect by ","change fisheye effect by ","change whirl effect by ","change pixelate effect by ","change mosaic effect by ","change brightness effect by ","change ghost effect by ","change pitch effect by ","change pan effect by ","set color effect to ","set fisheye effect to ","set whirl effect to ","set pixelate effect to ","set mosaic effect to ","set brightness effect to ","set ghost effect to ","set pitch effect to ","set pan effect to ","set drag mode to ","set rotation style to ",NULL};
        const char *pre=NULL;for(int i=0;arr[i];i++)if(fix_starts_with(core,arr[i])){pre=arr[i];break;} if(pre){char*tail=strdup(core+strlen(pre));int pct=(!strcmp(pre,"set size to ")||!strcmp(pre,"set volume to "));if(pct){char*q=strstr(tail," %");if(q)*q=0;}char*w=fix_wrap(tail);if(w){out=malloc(strlen(pre)+strlen(w)+(pct?2:0)+1);sprintf(out,"%s%s%s",pre,w,pct?" %":"");}free(tail);free(w);}
    }
    else if (!strncmp(core,"repeat until ",13) || !strncmp(core,"repeat ",7) || !strncmp(core,"if ",3)) { const char*pre=!strncmp(core,"repeat until ",13)?"repeat until ":(!strncmp(core,"repeat ",7)?"repeat ":"if "); char*w=fix_wrap(core+strlen(pre));if(w){out=malloc(strlen(pre)+strlen(w)+1);sprintf(out,"%s%s",pre,w);free(w);} }
    else if (!strncmp(core,"go forward ",11) || !strncmp(core,"go backward ",12)) { const char*pre=!strncmp(core,"go forward ",11)?"go forward ":"go backward ";char*tail=strdup(core+strlen(pre));char*q=strstr(tail," layers");if(q)*q=0;char*w=fix_wrap(tail);if(w){out=malloc(strlen(pre)+strlen(w)+8);sprintf(out,"%s%s layers",pre,w);}free(tail);free(w); }
    else if (!strncmp(core,"add ",4) && strstr(core," to list (")) { const char*mid=strstr(core," to list (");char*a=fix_trim_dup(core+4,(size_t)(mid-(core+4)));char*w=fix_wrap(a);if(w){out=malloc(strlen(w)+strlen(mid)+5);sprintf(out,"add %s%s",w,mid);}free(a);free(w); }
    else if (!strncmp(core,"delete ",7) && strstr(core," of list (")) { const char*mid=strstr(core," of list (");char*a=fix_trim_dup(core+7,(size_t)(mid-(core+7)));char*w=fix_wrap(a);if(w){out=malloc(strlen(w)+strlen(mid)+8);sprintf(out,"delete %s%s",w,mid);}free(a);free(w); }
    else if (!strncmp(core,"insert ",7) && strstr(core," at ") && strstr(core," of list (")) { const char*at=strstr(core," at "),*of=strstr(at+4," of list (");char*a=fix_trim_dup(core+7,(size_t)(at-(core+7))),*b=fix_trim_dup(at+4,(size_t)(of-(at+4)));char*wa=fix_wrap(a),*wb=fix_wrap(b);if(wa&&wb){out=malloc(strlen(wa)+strlen(wb)+strlen(of)+12);sprintf(out,"insert %s at %s%s",wa,wb,of);}free(a);free(b);free(wa);free(wb); }
    else if (!strncmp(core,"replace item ",13) && strstr(core," of list (") && strstr(core," with ")) { const char*of=strstr(core," of list ("),*with=strstr(of," with ");char*a=fix_trim_dup(core+13,(size_t)(of-(core+13))),*b=fix_trim_dup(with+6,strlen(with+6));char*wa=fix_wrap(a),*wb=fix_wrap(b);if(wa&&wb){char*mid=fix_trim_dup(of,(size_t)(with-of));out=malloc(strlen(wa)+strlen(mid)+strlen(wb)+20);sprintf(out,"replace item %s%s with %s",wa,mid,wb);free(mid);}free(a);free(b);free(wa);free(wb); }
    else if (!strncmp(core,"when (loudness > ) ",19) || !strncmp(core,"when (timer > ) ",16)) { const char*q=strrchr(core,')');if(q&&q[1]==' '){char*w=fix_wrap(q+2);if(w){size_t pre=(size_t)(q+2-core);out=malloc(pre+strlen(w)+1);memcpy(out,core,pre);strcpy(out+pre,w);free(w);}} }
    else { for(int i=0;i<procs->count&&!out;i++){const char*pn=procs->name[i];size_t m=strlen(pn);if(!strncmp(core,pn,m)&&(core[m]==0||core[m]==' ')){char*args=fix_wrap_all_exprs(core+m);if(args){out=malloc(m+strlen(args)+2);sprintf(out,"%s%s%s",pn,*args?" ":"",args);free(args);}}} }
    if(!out) out=strdup(core);
unchanged:
    { size_t n=ind+strlen(out?out:core)+2;char*r=malloc(n);snprintf(r,n,"%.*s%s\n",(int)ind,line,out?out:core);free(core);free(out);return r; }
}

static char *fix_project_jappl(const char *src, size_t len) {
    FixProcNames procs={0};const char*p=src;
    while(*p){const char*e=strchr(p,'\n');size_t n=e?(size_t)(e-p):strlen(p);char*line=fix_trim_dup(p,n);if(line&&!strncmp(line,"custom block (",14)){char*close=strchr(line+14,')');if(close){*close=0;fix_add_proc(&procs,line+14);}}free(line);if(!e)break;p=e+1;}
    size_t cap=len+len/3+4096,used=0;char*out=malloc(cap);if(!out){fix_free_procs(&procs);return NULL;}p=src;
    while(*p){const char*e=strchr(p,'\n');size_t n=e?(size_t)(e-p):strlen(p);char*fixed=fix_line(p,&procs);if(!fixed){free(out);fix_free_procs(&procs);return NULL;}size_t fl=strlen(fixed);if(used+fl+1>=cap){while(used+fl+1>=cap)cap*=2;out=realloc(out,cap);}memcpy(out+used,fixed,fl);used+=fl;free(fixed);if(!e)break;p=e+1;}
    out[used]=0;fix_free_procs(&procs);return out;
}

int decompile_sb3_compiler_syntax(const char *sb3_path, const char *out_zip_path) {
    int rc=decompile_sb3(sb3_path,out_zip_path);if(rc!=0)return rc;
    FixZipEntry*entries=NULL;int count=0;if(fix_read_zip(out_zip_path,&entries,&count)!=0)return -1;
    for(int i=0;i<count;i++)if(!strcmp(entries[i].name,"project.jappl")){char*fixed=fix_project_jappl((const char*)entries[i].data,entries[i].len);if(!fixed){fix_free_entries(entries,count);return -1;}free(entries[i].data);entries[i].data=(unsigned char*)fixed;entries[i].len=strlen(fixed);break;}
    rc=fix_write_zip(out_zip_path,entries,count);fix_free_entries(entries,count);return rc;
}
