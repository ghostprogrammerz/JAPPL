# JAPPL Known Parser Problems

Remaining issues found while compiling `jappl_kernel/project.jappl`.
All errors were reproduced with `./jappl2sb3 project.jappl out.sb3`.

---

## Bug 4 — `variable (x) contains (y)` inside compound `not (... and ...)` (line 134)

**Source line:**
```
if (not ((variable (word) contains (([)) and variable (word) contains ((])))) and not (((variable (word) = (text location)) or (variable (word) = (set variable)))))
```

**Errors:**
```
error at line 134: expected ')', got 'and'
error at line 134: expected ')', got 'variable'
```

**Root cause:**  
`parse_paren_expr` enters the structured-expr path and calls `parse_expr`. `parse_expr`
parses `variable (word) contains (([))` as left, sees `and` (a binop), then tries
`parse_atom` for the right side. `parse_atom` does not handle `variable (x) contains (y)`
as an atom — `contains` is handled as a suffix in `parse_expr` AFTER `parse_atom` returns,
but only at the top level of `parse_expr`, not when called recursively for the right-hand
side of `and`. So `variable (word)` is parsed as left of `and`, then `contains` appears
as the next token and is not consumed, causing `expect(')')` to see `and` instead.

**Fix needed:**  
In `parse_expr`, after calling `parse_atom` for the **right** side of a binop, also
check for a `contains` suffix on that result (same logic as the top-level contains check
at the end of `parse_expr`). Or factor `contains` handling into `parse_atom` directly
so it is always resolved before the caller sees it.

---

## Bug 5 — String literal used as `contains` target: `(0123456789.) contains (...)` (line 319)

**Source line:**
```
if ((0123456789.) contains (letter (variable (__i)) of (join(((input)) + (( ))))) and (0123456789.) contains (letter ((variable (__i) - (1))) of (join(((input)) + (( ))))))
```

**Errors:**
```
error at line 319: expected ')', got 'and'
error at line 319: unknown statement '0123456789.'
```

**Root cause:**  
`(0123456789.)` is a string literal (a digit-set used as a Scratch string for `contains`
checks). The lexer tokenizes `0123456789` as `TOK_NUMBER` and `.` as a separate token,
so `parse_paren_expr` parses `0123456789` as a number, then fails on `.`. Even if that
were fixed, `contains` appears *after* the paren closes, which is the same issue as Bug 4
— `(A) contains (B) and (C) contains (D)` fails because after `(A) contains (B)` is parsed,
`and` is a binop but the right side `(C) contains (D)` has the same `contains`-after-atom
problem.

**Fix needed (two parts):**  
1. `parse_paren_expr` raw-concat path: when the content starts with a digit, do NOT force
   it through the structured-expr path — let the raw concat assemble `0123456789.` as a
   string. Currently `TOK_NUMBER` in `is_structured_expr_start` causes it to be parsed as
   a number expression, losing the trailing `.`.  
   *Or*: in the number atom handler, if a `.` appears after the number and is followed by
   `)`, collect it into the string value.  
2. Same `contains`-as-binop-rhs fix as Bug 4.

---

## Bug 6 — `insert (() at (n) of list (x)` — `(()` treated as empty paren (line 334)

**Source line:**
```
insert (() at (1) of list (__i)
```

**Errors:**
```
error at line 334: expected ')', got 'at'
error at line 334: expected 'at', got '('
```

**Root cause:**  
`insert` handler calls `parse_paren_expr` for the value. `(()` — outer `(` consumed, next
token is `(` again (structured start), so it calls `parse_expr` → `parse_atom` → `(` →
`parse_paren_expr` again → inner `(` consumed, sees `)` immediately → returns empty/zero
expr → `expect(')')` for the inner close → OK → back in outer `parse_paren_expr` →
`expect(')')` → sees `at` → error.

The value `(()` is Scratch for the empty string `""` — Scratch uses `(())` (double parens,
empty inner) to represent an empty string slot. The outer `(` is the slot wrapper and the
inner `(` is the empty value. Parser sees three `(` and only two `)` in `(()` and miscounts.

Also present: `delete item num (()) of list (__i) of list (__i)` — the double-list suffix
`of list (x) of list (x)` is not handled (line 150, 331). Parser stops after the first
`of list (x)` and leaves `of list (x)` unconsumed.

**Fix needed:**  
`insert` (and `delete item num`, `replace item`) handlers: after reading the list name,
consume a second optional `of list (name)` suffix if present (Scratch nested list pattern).  
For `(()` empty-string: `parse_paren_expr` should detect the `(` `(` `)` `)` pattern and
return an empty `EXPR_STRING` rather than recursing.

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
