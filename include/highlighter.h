#ifndef VIDEO_REDAC_HIGHLIGHTER_H
#define VIDEO_REDAC_HIGHLIGHTER_H

/*
 * highlighter.h — syntax highlighting for C / Go / Rust / Python.
 *
 * Design: the lexer works *line by line* and carries its state in
 * `HighlightState`. That is what lets it handle multi-line constructs (C block
 * comments, Python triple-quoted strings) without building a tree for the
 * whole file at once.
 *
 * The lexer allocates *nothing*: the caller supplies the buffer. It can
 * therefore run off the arena and needs no cleanup of its own.
 *
 * Tokens are `contiguous` — they cover every byte of the line, whitespace
 * included (as TOK_TEXT). That keeps rendering to a straight walk: draw each
 * token in turn and accumulate x.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LANG_NONE = 0,
    LANG_C,       /* C, C++ and near-enough syntaxes */
    LANG_GO,
    LANG_RUST,
    LANG_PYTHON
} Language;

typedef enum {
    TOK_TEXT = 0,   /* whitespace and anything unclassified */
    TOK_KEYWORD,
    TOK_TYPE,
    TOK_STRING,
    TOK_NUMBER,     /* numbers and constants (true/false/nil/None) */
    TOK_COMMENT,
    TOK_FUNCTION,   /* an identifier followed by '(' */
    TOK_OPERATOR,
    TOK_PUNCT,
    TOK_PREPROC,    /* #include, #define */
    TOK_CLASS_COUNT
} TokenClass;

typedef struct {
    size_t     start;  /* byte offset from the start of the line */
    size_t     len;
    TokenClass cls;
} Token;

/* Lexer state carried between lines. Zeroed = start of a new file. */
typedef struct {
    int  mode;          /* internal: 0=normal, 1=block comment, 2=triple-quoted */
    char triple_quote;  /* Python: which quote opened it (" or ') */
} HighlightState;

/* "c", "go", "rust", "python", "py"… → Language. Unknown → LANG_NONE. */
Language highlighter_language_from_name(const char *name);

/*
 * Splits one line into tokens.
 *
 * `out` — the caller's buffer of `max` entries; returns how many were written.
 * If the buffer runs out, the remaining text collapses into a single TOK_TEXT
 * token — so the output is always complete and highlighting can never "lose"
 * characters.
 */
size_t highlighter_tokenize_line(const char *line, Language lang, HighlightState *state,
                                 Token *out, size_t max);

/* Colour for a token class (Catppuccin Mocha). */
Color highlighter_class_color(TokenClass cls);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_HIGHLIGHTER_H */
