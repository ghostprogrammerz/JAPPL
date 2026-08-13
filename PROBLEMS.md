# JAPPL Known Parser Problems

Remaining issues found while compiling `jappl_kernel/project.jappl`.
All errors were reproduced with `./jappl2sb3 project.jappl out.sb3`.

---

## Previously fixed (this session)

| Bug | Description | Status |
|-----|-------------|--------|
| 1 | `if on edge bounce` dead code — if-handler consumed `if` before edge-bounce check | Fixed |
| 2 | `create clone of` — `of` checked as `TOK_IDENT` but is `TOK_OF` | Fixed |
| 3 | `set rotation style to (left-right)` — spaces inserted around `-` in `read_name_until` | Fixed |
| — | `--` inside strings parsed as `--global` flag | Fixed (lexer: `--` only a flag when followed by alpha) |
| — | `TOK_LIST` missing from `is_structured_expr_start` | Fixed |
| — | `add bare-expr to list` — `add` called `parse_paren_expr` for unparenthesized expressions | Fixed |
| — | `read_current_name` consuming `TOK_TO` as part of reporter name | Fixed |
| — | `"` inside parens lexed as string delimiter with no closing `"` | Fixed (lookahead) |
| — | `([)` — `[` inside parens treated as `[varname]` variable syntax | Fixed |
| — | Server blocking: `package_sb3` ran `node` synchronously, causing fetch timeout | Fixed (async fork) |
| — | IDE port mismatch: default backend URL was 8765, `make run` uses 8080 | Fixed |
| 4 | `variable (x) contains (y)` on RHS of `and`/`or` — `contains` not resolved after `parse_atom` for binop RHS | Fixed: factored into `maybe_contains()`, applied to both LHS atom and RHS atom in `parse_expr` |
| 5 | `(0123456789.) contains (...)` — digit-string split by lexer; structured-expr path taken on leading `TOK_NUMBER` | Fixed: `parse_paren_expr` detects `TOK_NUMBER` not followed by `)` or binop and falls through to raw-string concat; `contains`-on-RHS fixed by Bug 4 fix |
| 6 | `insert (() at ...` — `(()` empty-string miscounts parens; `of list (x) of list (x)` double suffix unconsumed | Fixed: `parse_paren_expr` detects `( (` `)` pattern and returns empty `EXPR_STRING`; `delete`/`insert`/`replace` handlers consume optional second `of list (name)` suffix |
