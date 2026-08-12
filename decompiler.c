#define decompile_sb3 decompile_sb3_raw
#include "decompiler/decompiler.c"
#undef decompile_sb3

#define decompile_sb3 decompile_sb3_raw
#include "decompiler/decompiler_fix.c"
#undef decompile_sb3

int decompile_sb3(const char *sb3_path, const char *out_zip_path) {
    return decompile_sb3_compiler_syntax(sb3_path, out_zip_path);
}
