#ifndef EMITTER_H
#define EMITTER_H

#include "ast.h"

/* ── Initial variable / list state (from a decompiler zip) ──────── */
typedef struct { char *name; char *value; } StateVar;
typedef struct { char *name; char **items; int count; } StateList;

typedef struct {
    StateVar  *vars;  int var_count;
    StateList *lists; int list_count;
} StateTable;

/* Look up a var value; returns NULL if not in table */
const char *state_get_var(const StateTable *st, const char *name);
/* Look up a list; returns NULL if not in table */
const StateList *state_get_list(const StateTable *st, const char *name);
void state_table_free(StateTable *st);

/* Emits project.json into buf (caller frees).
   Returns 0 on success, -1 on error. */
int emit_project_json(Program *prog, char **buf_out);
int emit_project_json_with_state(Program *prog, char **buf_out, const StateTable *st);

/* Writes the .sb3 zip to path.
   Returns 0 on success, -1 on error. */
int emit_sb3(Program *prog, const char *path);
int emit_sb3_with_state(Program *prog, const char *path, const StateTable *st);

#endif
