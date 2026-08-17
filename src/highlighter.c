/*
 * highlighter.c — a simple, allocation-free lexer.
 *
 * This is not a full parser and is not trying to be one: "visually correct"
 * colouring is all a video needs. Hence a classic scanner over keyword
 * tables — no AST, no grammar.
 */

#include "highlighter.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Language dictionaries                                                      */
/* ------------------------------------------------------------------------- */

/* NULL-terminated tables — the search is linear, but the words are short and
 * the whole rasterization happens only once anyway. */

static const char *const C_KEYWORDS[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "goto", "sizeof", "typedef", "struct", "union", "enum",
    "static", "extern", "const", "volatile", "inline", "register", "restrict",
    "auto", "_Alignof", "_Atomic", "_Static_assert", "_Thread_local", "namespace",
    "class", "public", "private", "protected", "template", "typename", "new",
    "delete", "using", "virtual", "override", "constexpr", "nullptr", NULL
};
static const char *const C_TYPES[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "bool", "size_t", "ssize_t", "ptrdiff_t", "int8_t", "int16_t",
    "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "FILE", "va_list", "wchar_t", NULL
};
static const char *const C_CONSTANTS[] = { "true", "false", "NULL", "nullptr", NULL };

static const char *const GO_KEYWORDS[] = {
    "break", "case", "chan", "const", "continue", "default", "defer", "else",
    "fallthrough", "for", "func", "go", "goto", "if", "import", "interface",
    "map", "package", "range", "return", "select", "struct", "switch", "type",
    "var", NULL
};
static const char *const GO_TYPES[] = {
    "bool", "byte", "complex64", "complex128", "error", "float32", "float64",
    "int", "int8", "int16", "int32", "int64", "rune", "string", "uint", "uint8",
    "uint16", "uint32", "uint64", "uintptr", "any", NULL
};
static const char *const GO_CONSTANTS[] = { "true", "false", "nil", "iota", NULL };

static const char *const RUST_KEYWORDS[] = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else",
    "enum", "extern", "fn", "for", "if", "impl", "in", "let", "loop", "match",
    "mod", "move", "mut", "pub", "ref", "return", "static", "struct", "super",
    "trait", "type", "unsafe", "use", "where", "while", "Self", "self", NULL
};
static const char *const RUST_TYPES[] = {
    "bool", "char", "f32", "f64", "i8", "i16", "i32", "i64", "i128", "isize",
    "str", "String", "u8", "u16", "u32", "u64", "u128", "usize", "Vec", "Option",
    "Result", "Box", "Rc", "Arc", "HashMap", "Some", "None", "Ok", "Err", NULL
};
static const char *const RUST_CONSTANTS[] = { "true", "false", NULL };

static const char *const PY_KEYWORDS[] = {
    "and", "as", "assert", "async", "await", "break", "class", "continue", "def",
    "del", "elif", "else", "except", "finally", "for", "from", "global", "if",
    "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise",
    "return", "try", "while", "with", "yield", "match", "case", NULL
};
static const char *const PY_TYPES[] = {
    "int", "float", "str", "bool", "list", "dict", "set", "tuple", "bytes",
    "object", "len", "range", "print", "enumerate", "zip", "map", "filter",
    "open", "sum", "min", "max", "sorted", "abs", "self", NULL
};
static const char *const PY_CONSTANTS[] = { "True", "False", "None", NULL };

/* The complete description of one language. */
typedef struct {
    const char *const *keywords;
    const char *const *types;
    const char *const *constants;
    const char        *line_comment;   /* "//" or "#" */
    bool               block_comments; /* C-style block comments */
    bool               preproc;        /* '#' at line start = a directive */
    bool               triple_strings;  /* Python's """ */
    bool               raw_backtick;    /* Go's `raw string` */
} LangSpec;

static LangSpec lang_spec(Language lang)
{
    LangSpec s;
    memset(&s, 0, sizeof s);

    switch (lang) {
        case LANG_C:
            s.keywords = C_KEYWORDS; s.types = C_TYPES; s.constants = C_CONSTANTS;
            s.line_comment = "//"; s.block_comments = true; s.preproc = true;
            break;
        case LANG_GO:
            s.keywords = GO_KEYWORDS; s.types = GO_TYPES; s.constants = GO_CONSTANTS;
            s.line_comment = "//"; s.block_comments = true; s.raw_backtick = true;
            break;
        case LANG_RUST:
            s.keywords = RUST_KEYWORDS; s.types = RUST_TYPES; s.constants = RUST_CONSTANTS;
            s.line_comment = "//"; s.block_comments = true;
            break;
        case LANG_PYTHON:
            s.keywords = PY_KEYWORDS; s.types = PY_TYPES; s.constants = PY_CONSTANTS;
            s.line_comment = "#"; s.triple_strings = true;
            break;
        case LANG_NONE:
        default:
            break;
    }
    return s;
}

Language highlighter_language_from_name(const char *name)
{
    if (name == NULL) {
        return LANG_NONE;
    }

    /* Case-insensitive comparison against a short table. */
    static const struct { const char *name; Language lang; } table[] = {
        { "c", LANG_C },     { "h", LANG_C },      { "cpp", LANG_C },
        { "c++", LANG_C },   { "cc", LANG_C },     { "hpp", LANG_C },
        { "cuda", LANG_C },  { "cu", LANG_C },     { "java", LANG_C },
        { "js", LANG_C },    { "javascript", LANG_C }, { "ts", LANG_C },
        { "go", LANG_GO },   { "golang", LANG_GO },
        { "rust", LANG_RUST }, { "rs", LANG_RUST },
        { "python", LANG_PYTHON }, { "py", LANG_PYTHON },
    };

    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        const char *a = name, *b = table[i].name;
        while (*a && *b) {
            int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
            if (ca != *b) break;
            a++; b++;
        }
        if (*a == '\0' && *b == '\0') {
            return table[i].lang;
        }
    }
    return LANG_NONE;
}

/* ------------------------------------------------------------------------- */
/* Character classes                                                          */
/* ------------------------------------------------------------------------- */

static bool is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           (unsigned char)c >= 0x80; /* UTF-8 bytes count as identifier chars */
}

static bool is_ident_char(char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool in_word_list(const char *const *list, const char *word, size_t len)
{
    if (list == NULL) {
        return false;
    }
    for (size_t i = 0; list[i] != NULL; i++) {
        if (strlen(list[i]) == len && strncmp(list[i], word, len) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Emitting tokens                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    Token *out;
    size_t max;
    size_t count;
} Emitter;

static void emit(Emitter *e, size_t start, size_t len, TokenClass cls)
{
    if (len == 0) {
        return;
    }

    /*
     * When the buffer runs out we extend the last token rather than dropping
     * text. Colouring degrades; text never disappears.
     */
    if (e->count >= e->max) {
        if (e->count > 0) {
            Token *last = &e->out[e->count - 1];
            last->len   = start + len - last->start;
        }
        return;
    }

    /* Merge adjacent tokens of the same class — fewer Cairo calls. */
    if (e->count > 0) {
        Token *last = &e->out[e->count - 1];
        if (last->cls == cls && last->start + last->len == start) {
            last->len += len;
            return;
        }
    }

    e->out[e->count].start = start;
    e->out[e->count].len   = len;
    e->out[e->count].cls   = cls;
    e->count++;
}

/* ------------------------------------------------------------------------- */
/* Lexer                                                                      */
/* ------------------------------------------------------------------------- */

/* Finds the end of a string literal, honouring escapes. */
static size_t scan_string(const char *line, size_t i, size_t len, char quote, bool allow_escape)
{
    i++; /* the opening quote */
    while (i < len) {
        if (allow_escape && line[i] == '\\' && i + 1 < len) {
            i += 2; /* \" and \\ do not close the literal */
            continue;
        }
        if (line[i] == quote) {
            return i + 1; /* just past the closer */
        }
        i++;
    }
    return len; /* unterminated — run to the end of the line */
}

size_t highlighter_tokenize_line(const char *line, Language lang, HighlightState *state,
                                 Token *out, size_t max)
{
    Emitter e;
    e.out   = out;
    e.max   = max;
    e.count = 0;

    if (line == NULL || out == NULL || max == 0) {
        return 0;
    }

    LangSpec spec = lang_spec(lang);
    size_t   len  = strlen(line);
    size_t   i    = 0;

    HighlightState local;
    local.mode = 0;
    local.triple_quote = 0;
    if (state == NULL) {
        state = &local;
    }

    /* --- states carried over from the previous line ---------------------- */

    if (state->mode == 1) { /* inside a block comment */
        const char *end = strstr(line, "*/");
        if (end == NULL) {
            emit(&e, 0, len, TOK_COMMENT);
            return e.count; /* mode unchanged — the comment continues */
        }
        size_t stop = (size_t)(end - line) + 2;
        emit(&e, 0, stop, TOK_COMMENT);
        state->mode = 0;
        i = stop;
    } else if (state->mode == 2) { /* inside a triple-quoted string */
        char triple[4] = { state->triple_quote, state->triple_quote, state->triple_quote, '\0' };
        const char *end = strstr(line, triple);
        if (end == NULL) {
            emit(&e, 0, len, TOK_STRING);
            return e.count;
        }
        size_t stop = (size_t)(end - line) + 3;
        emit(&e, 0, stop, TOK_STRING);
        state->mode = 0;
        i = stop;
    }

    /* --- C preprocessor: '#' is the first non-space character ------------- */
    if (spec.preproc && i == 0) {
        size_t j = 0;
        while (j < len && (line[j] == ' ' || line[j] == '\t')) {
            j++;
        }
        if (j < len && line[j] == '#') {
            emit(&e, 0, j, TOK_TEXT);
            emit(&e, j, len - j, TOK_PREPROC);
            return e.count;
        }
    }

    /* --- the main scanner -------------------------------------------------- */
    while (i < len) {
        char c = line[i];

        /* whitespace */
        if (c == ' ' || c == '\t') {
            size_t start = i;
            while (i < len && (line[i] == ' ' || line[i] == '\t')) {
                i++;
            }
            emit(&e, start, i - start, TOK_TEXT);
            continue;
        }

        /* line comment */
        if (spec.line_comment != NULL) {
            size_t lc = strlen(spec.line_comment);
            if (i + lc <= len && strncmp(line + i, spec.line_comment, lc) == 0) {
                emit(&e, i, len - i, TOK_COMMENT);
                break;
            }
        }

        /* start of a block comment */
        if (spec.block_comments && i + 1 < len && line[i] == '/' && line[i + 1] == '*') {
            const char *end = strstr(line + i + 2, "*/");
            if (end == NULL) {
                emit(&e, i, len - i, TOK_COMMENT);
                state->mode = 1; /* continues on the next line */
                break;
            }
            size_t stop = (size_t)(end - line) + 2;
            emit(&e, i, stop - i, TOK_COMMENT);
            i = stop;
            continue;
        }

        /* Python triple quotes */
        if (spec.triple_strings && i + 2 < len &&
            (c == '"' || c == '\'') && line[i + 1] == c && line[i + 2] == c) {
            char        triple[4] = { c, c, c, '\0' };
            const char *end       = strstr(line + i + 3, triple);
            if (end == NULL) {
                emit(&e, i, len - i, TOK_STRING);
                state->mode         = 2;
                state->triple_quote = c;
                break;
            }
            size_t stop = (size_t)(end - line) + 3;
            emit(&e, i, stop - i, TOK_STRING);
            i = stop;
            continue;
        }

        /* ordinary strings and character literals */
        if (c == '"' || c == '\'') {
            size_t stop = scan_string(line, i, len, c, true);
            emit(&e, i, stop - i, TOK_STRING);
            i = stop;
            continue;
        }

        /* Go raw string — escapes do not apply */
        if (spec.raw_backtick && c == '`') {
            size_t stop = scan_string(line, i, len, '`', false);
            emit(&e, i, stop - i, TOK_STRING);
            i = stop;
            continue;
        }

        /* numbers (including 0x1F, 1.5e-3, 1_000, 42u) */
        if (is_digit(c) || (c == '.' && i + 1 < len && is_digit(line[i + 1]))) {
            size_t start = i;
            while (i < len && (is_ident_char(line[i]) || line[i] == '.')) {
                /* exponent sign: 1e-5 */
                if ((line[i] == 'e' || line[i] == 'E') && i + 1 < len &&
                    (line[i + 1] == '+' || line[i + 1] == '-')) {
                    i += 2;
                    continue;
                }
                i++;
            }
            emit(&e, start, i - start, TOK_NUMBER);
            continue;
        }

        /* identifiers and keywords */
        if (is_ident_start(c)) {
            size_t start = i;
            while (i < len && is_ident_char(line[i])) {
                i++;
            }
            size_t      wlen = i - start;
            const char *word = line + start;

            TokenClass cls;
            if (in_word_list(spec.keywords, word, wlen)) {
                cls = TOK_KEYWORD;
            } else if (in_word_list(spec.types, word, wlen)) {
                cls = TOK_TYPE;
            } else if (in_word_list(spec.constants, word, wlen)) {
                cls = TOK_NUMBER;
            } else {
                /* Heuristic: a name followed by '(' is a function call. */
                size_t j = i;
                while (j < len && (line[j] == ' ' || line[j] == '\t')) {
                    j++;
                }
                cls = (j < len && line[j] == '(') ? TOK_FUNCTION : TOK_TEXT;
            }
            emit(&e, start, wlen, cls);
            continue;
        }

        /* operators */
        if (strchr("+-*/%=<>!&|^~?:", c) != NULL) {
            size_t start = i;
            while (i < len && strchr("+-*/%=<>!&|^~?:", line[i]) != NULL) {
                i++;
            }
            emit(&e, start, i - start, TOK_OPERATOR);
            continue;
        }

        /* punctuation */
        if (strchr("(){}[];,.", c) != NULL) {
            emit(&e, i, 1, TOK_PUNCT);
            i++;
            continue;
        }

        /* everything else — neutral text (UTF-8 bytes included). */
        emit(&e, i, 1, TOK_TEXT);
        i++;
    }

    return e.count;
}

/* ------------------------------------------------------------------------- */
/* Theme                                                                      */
/* ------------------------------------------------------------------------- */

Color highlighter_class_color(TokenClass cls)
{
    /* Catppuccin Mocha — good contrast on the dark #1E1E2E panel. */
    switch (cls) {
        case TOK_KEYWORD:  return (Color){ 0xCB, 0xA6, 0xF7, 0xFF }; /* mauve  */
        case TOK_TYPE:     return (Color){ 0xF9, 0xE2, 0xAF, 0xFF }; /* yellow */
        case TOK_STRING:   return (Color){ 0xA6, 0xE3, 0xA1, 0xFF }; /* green  */
        case TOK_NUMBER:   return (Color){ 0xFA, 0xB3, 0x87, 0xFF }; /* peach  */
        case TOK_COMMENT:  return (Color){ 0x6C, 0x70, 0x86, 0xFF }; /* overlay*/
        case TOK_FUNCTION: return (Color){ 0x89, 0xB4, 0xFA, 0xFF }; /* blue   */
        case TOK_OPERATOR: return (Color){ 0x89, 0xDC, 0xEB, 0xFF }; /* sky    */
        case TOK_PUNCT:    return (Color){ 0xBA, 0xC2, 0xDE, 0xFF }; /* subtext*/
        case TOK_PREPROC:  return (Color){ 0xF5, 0xC2, 0xE7, 0xFF }; /* pink   */
        case TOK_TEXT:
        default:           return (Color){ 0xCD, 0xD6, 0xF4, 0xFF }; /* text   */
    }
}
