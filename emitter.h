#ifndef EMITTER_H
#define EMITTER_H

#include "ast.h"

/* Emits project.json into buf (caller frees).
   Returns 0 on success, -1 on error. */
int emit_project_json(Program *prog, char **buf_out);

/* Writes the .sb3 zip to path.
   Returns 0 on success, -1 on error. */
int emit_sb3(Program *prog, const char *path);

#endif
