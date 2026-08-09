#ifndef DECOMPILER_H
#define DECOMPILER_H

/* Reads an .sb3 file and writes a .jappl source file.
   Returns 0 on success, -1 on error. */
int decompile_sb3(const char *sb3_path, const char *jappl_path);

#endif
