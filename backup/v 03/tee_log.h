#ifndef TEE_LOG_H
#define TEE_LOG_H

#include <stdio.h>
#include <stdarg.h>

// ─── TEE LOGGING ─────────────────────────────────────────────────────────────
// Writes to both the terminal (original fd) AND the log file simultaneously.
// Use tee_out() instead of printf(), tee_err() instead of fprintf(stderr,...).

static FILE *_tee_log_file = NULL;  // log file handle
static FILE *_tee_err_file = NULL;  // err file handle
static FILE *_tee_terminal = NULL;  // saved original stdout (terminal)
static FILE *_tee_terminal_err = NULL; // saved original stderr (terminal)

// Call once in main() BEFORE freopen — saves the terminal fd
static inline void tee_init(FILE *log_file, FILE *err_file,
                             FILE *terminal_out, FILE *terminal_err) {
    _tee_log_file    = log_file;
    _tee_err_file    = err_file;
    _tee_terminal    = terminal_out;
    _tee_terminal_err = terminal_err;
}

// printf equivalent — writes to terminal + log file
static inline void tee_out(const char *fmt, ...) {
    va_list args;

    if (_tee_terminal) {
        va_start(args, fmt);
        vfprintf(_tee_terminal, fmt, args);
        va_end(args);
        fflush(_tee_terminal);
    }

    if (_tee_log_file) {
        va_start(args, fmt);
        vfprintf(_tee_log_file, fmt, args);
        va_end(args);
        fflush(_tee_log_file);
    }
}

// fprintf(stderr,...) equivalent — writes to terminal stderr + err file
static inline void tee_err(const char *fmt, ...) {
    va_list args;

    if (_tee_terminal_err) {
        va_start(args, fmt);
        vfprintf(_tee_terminal_err, fmt, args);
        va_end(args);
        fflush(_tee_terminal_err);
    }

    if (_tee_err_file) {
        va_start(args, fmt);
        vfprintf(_tee_err_file, fmt, args);
        va_end(args);
        fflush(_tee_err_file);
    }
}

#endif // TEE_LOG_H
