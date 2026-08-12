#ifndef AST_H
#define AST_H

/* ── Expression node types ───────────────────────────────────────── */
typedef enum {
    EXPR_NUMBER,
    EXPR_STRING,
    EXPR_VAR,          /* [name] */
    EXPR_LIST_ITEM,    /* item (expr) of <list> */
    EXPR_LIST_LENGTH,  /* length of <list> */
    EXPR_LIST_CONTAINS,/* <list> contains (expr) */
    EXPR_BINOP,        /* a OP b */
    EXPR_NOT,          /* not (expr) */
    EXPR_PICK_RANDOM,  /* pick random (a) to (b) */
    EXPR_JOIN,         /* join (a) (b) */
    EXPR_LETTER_OF,    /* letter (n) of (str) */
    EXPR_LENGTH_OF,    /* length of (str) */
    EXPR_STR_CONTAINS, /* (str) contains (sub) */
    EXPR_ROUND,
    EXPR_MATH_FN,      /* abs/sqrt/floor/ceiling/sin/cos/tan */
    EXPR_MOUSE_X,      /* sensing_mousex  — cursor x position */
    EXPR_MOUSE_Y,      /* sensing_mousey  — cursor y position */
    EXPR_X_POS,        /* motion_xposition — sprite x position */
    EXPR_Y_POS,        /* motion_yposition — sprite y position */
    EXPR_ANSWER,
    EXPR_TIMER,
    EXPR_MOUSE_DOWN,
    EXPR_KEY_PRESSED,      /* key (name) pressed */
    EXPR_TOUCHING,         /* touching (target) */
    EXPR_TOUCHING_COLOR,   /* touching color (#rrggbb) */
    EXPR_DISTANCE_TO,      /* distance to (target) */
    EXPR_RANDOM_POSITION,  /* special target value */
    EXPR_MOUSE_POINTER,    /* special target value */
    EXPR_EDGE,             /* special target value */
    EXPR_ARG_REPORTER,     /* procedure argument reporter — str = param name */
    EXPR_ARG_REPORTER_BOOL,/* boolean argument reporter — str = param name */

    /* motion reporters */
    EXPR_DIRECTION,        /* direction */

    /* looks reporters */
    EXPR_SIZE,             /* size (%) */
    EXPR_COSTUME_NUM,      /* costume number */
    EXPR_COSTUME_NAME,     /* costume name */
    EXPR_BACKDROP_NUM,     /* backdrop number */
    EXPR_BACKDROP_NAME,    /* backdrop name */

    /* sound reporters */
    EXPR_VOLUME,           /* volume */

    /* sensing reporters — EXPR_TOUCHING_COLOR already defined above */
    EXPR_COLOR_TOUCHING_COLOR,  /* pair.a/b = color expr nodes (EXPR_STRING) */
    EXPR_LOUDNESS,
    EXPR_USERNAME,
    EXPR_ONLINE,
    EXPR_CURRENT,          /* str = "year"/"month"/"date"/"day of week"/"hour"/"minute"/"second" */
    EXPR_DAYS_SINCE_2000,
    EXPR_SENSING_OF,       /* sensing_of.sprite + sensing_of.property */

    /* list reporters */
    EXPR_LIST_ITEM_NUM,    /* list_item.list + list_item.index (reuses list_item union field) */
} ExprType;

typedef struct Expr Expr;
struct Expr {
    ExprType type;
    int      line;
    union {
        double      number;
        char       *str;       /* STRING, VAR name, LIST name, key name,
                                  sprite name, math_fn name, color */
        struct { Expr *left; Expr *right; char op[8]; } binop;
        struct { Expr *expr; }                           unary;
        struct { Expr *a; Expr *b; }                     pair;
        struct { char *fn; Expr *arg; }                  mathfn;
        struct { char *list; Expr *index; }              list_item;
        struct { char *list; Expr *val; }                list_contains;
        struct { char *list; }                           list_len;
        struct { Expr *str; Expr *sub; }                 str_contains;
        struct { Expr *str; Expr *n; }                   letter_of;
        struct { char *target; }                         touching;
        struct { char *sprite; char *property; }         sensing_of;
    };
};

/* ── Statement node types ────────────────────────────────────────── */
typedef enum {
    /* control */
    STMT_FOREVER,
    STMT_REPEAT,
    STMT_IF,
    STMT_IF_ELSE,
    STMT_WAIT,
    STMT_WAIT_UNTIL,
    STMT_STOP,          /* stop all / this script / other scripts */
    STMT_REPEAT_UNTIL,  /* repeat until (cond) { } */

    /* motion */
    STMT_MOVE_STEPS,
    STMT_TURN_RIGHT,
    STMT_TURN_LEFT,
    STMT_GOTO_XY,
    STMT_GOTO_TARGET,   /* random position / mouse pointer */
    STMT_GLIDE_XY,
    STMT_GLIDE_TARGET,
    STMT_SET_X,
    STMT_SET_Y,
    STMT_CHANGE_X,
    STMT_CHANGE_Y,
    STMT_POINT_DIR,
    STMT_POINT_TOWARDS,

    /* looks */
    STMT_SAY,
    STMT_SAY_SECS,
    STMT_THINK,
    STMT_THINK_SECS,
    STMT_SWITCH_COSTUME,
    STMT_NEXT_COSTUME,
    STMT_SWITCH_BACKDROP,
    STMT_NEXT_BACKDROP,       /* next backdrop */
    STMT_SET_SIZE,
    STMT_CHANGE_SIZE,
    STMT_SHOW,
    STMT_HIDE,
    STMT_CHANGE_EFFECT,       /* change [effect] effect by (value) */
    STMT_SET_EFFECT,          /* set [effect] effect to (value) */
    STMT_CLEAR_EFFECTS,       /* clear graphic effects */
    STMT_GOTO_FRONT_BACK,     /* go to [front/back] layer */
    STMT_GOTO_LAYER,          /* go [forward/backward] (n) layers */

    /* sound */
    STMT_PLAY_SOUND,
    STMT_PLAY_SOUND_UNTIL,
    STMT_STOP_SOUNDS,
    STMT_SET_VOLUME,
    STMT_CHANGE_VOLUME,
    STMT_CHANGE_SOUND_EFFECT, /* change [pitch/pan] effect by (value) */
    STMT_SET_SOUND_EFFECT,    /* set [pitch/pan] effect to (value) */
    STMT_CLEAR_SOUND_EFFECTS, /* clear sound effects */

    /* events */
    STMT_BROADCAST,
    STMT_BROADCAST_WAIT,

    /* sensing */
    STMT_ASK,
    STMT_RESET_TIMER,

    /* variables */
    STMT_SET_VAR,
    STMT_CHANGE_VAR,
    STMT_SHOW_VAR,
    STMT_HIDE_VAR,

    /* custom blocks */
    STMT_PROC_CALL,        /* name (arg1) (arg2) ... */

    /* lists */
    STMT_LIST_ADD,
    STMT_LIST_DELETE,
    STMT_LIST_DELETE_ALL,
    STMT_LIST_INSERT,
    STMT_LIST_REPLACE,
    STMT_SHOW_LIST,
    STMT_HIDE_LIST,

    /* motion extras */
    STMT_IF_ON_EDGE_BOUNCE,
    STMT_SET_ROTATION_STYLE,  /* s->name = "left-right"/"don't rotate"/"all around" */

    /* clone control */
    STMT_CREATE_CLONE,        /* s->target = sprite name or "_myself_" */
    STMT_DELETE_CLONE,

    /* sensing */
    STMT_SET_DRAG_MODE,       /* s->name = "draggable"/"not draggable" */
} StmtType;

typedef struct Stmt Stmt;
struct Stmt {
    StmtType type;
    int      line;

    /* body for control blocks */
    Stmt   **body;
    int      body_count;
    Stmt   **else_body;
    int      else_count;

    /* condition / count / time */
    Expr    *cond;
    Expr    *count;
    Expr    *secs;

    /* generic args (up to 3) */
    Expr    *a, *b, *c;

    /* string args (names) */
    char    *name;      /* var, list, costume, backdrop, sprite, stop-kind,
                           effect name, layer direction, or procedure name */
    char    *target;    /* motion targets: "random position", "mouse pointer",
                           sprite name, "edge" */

    /* STMT_PROC_CALL argument list */
    Expr   **args;
    int      arg_count;
};

/* ── Hat / Script ────────────────────────────────────────────────── */
typedef enum {
    HAT_GREEN_FLAG,
    HAT_MESSAGE,
    HAT_KEY_PRESSED,
    HAT_SPRITE_CLICKED,
    HAT_PROCEDURE_DEF,      /* define (name) { } */
    HAT_NONE,               /* floating */
    HAT_CLONE_START,        /* when (start as clone) */
    HAT_BACKDROP_SWITCHES,  /* when (backdrop switches to X) — hat_arg = backdrop name */
    HAT_GREATER_THAN,       /* when (loudness/timer > N) — hat_arg="LOUDNESS"/"TIMER" */
} HatType;

typedef struct {
    HatType  hat;
    char    *hat_arg;   /* message name, key name, or procedure name */
    int      line;
    Stmt   **body;
    int      body_count;
    /* HAT_PROCEDURE_DEF metadata */
    int      no_refresh;   /* run without screen refresh */
    char   **proc_params;  /* param names in order */
    int      proc_param_count;
    /* HAT_GREATER_THAN threshold */
    Expr    *hat_threshold;
} Script;

/* ── Procedure parameter ─────────────────────────────────────────── */
typedef struct {
    char *name;    /* param name as seen by arg reporters */
} ProcParam;

/* ── Procedure declaration (forward decl within a sprite) ────────── */
typedef struct {
    char       *name;
    ProcParam **params;
    int         param_count;
    int         no_refresh;   /* 1 = run without screen refresh */
    int         line;
} ProcDecl;

/* ── Variable / List declaration ─────────────────────────────────── */
typedef struct {
    char *name;
    Expr *init;        /* NULL if no initializer */
    int   is_global;   /* 1 if --global flag present */
    int   line;
} VarDecl;

typedef struct {
    char *name;
    int   is_global;
    int   line;
} ListDecl;

/* ── Sprite ──────────────────────────────────────────────────────── */
typedef struct {
    char      *name;       /* NULL = stage */
    int        is_stage;
    VarDecl  **vars;
    int        var_count;
    ListDecl **lists;
    int        list_count;
    Script   **scripts;
    int        script_count;
    ProcDecl **procs;      /* custom block declarations */
    int        proc_count;
} Sprite;

/* ── Program (root) ──────────────────────────────────────────────── */
typedef struct {
    VarDecl  **globals;
    int        global_count;
    ListDecl **global_lists;
    int        global_list_count;
    Sprite   **sprites;
    int        sprite_count;
} Program;

/* ── Constructors / destructors ─────────────────────────────────── */
Expr    *expr_new(ExprType type, int line);
Stmt    *stmt_new(StmtType type, int line);
Script  *script_new(HatType hat, const char *hat_arg, int line);
Sprite  *sprite_new(const char *name, int is_stage);
Program *program_new(void);
void     program_free(Program *p);

#endif
