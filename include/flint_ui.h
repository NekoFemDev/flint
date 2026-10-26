/*
 * flint_ui.h — ANSI SGR color macros for Flint's terminal output.
 *
 * Plain escape sequences, not libc calls: every writer decides for itself
 * when color is appropriate (interactive terminal, NO_COLOR unset) and
 * wraps text in these. Current users: the assembler's diagnostics
 * (src/flint_asm.c, report_diag) and the CLI's status lines.
 *
 * Palette: SGR 30..37 foreground colors, 90 bright black (gray), 1 bold.
 */
#ifndef FLINT_UI_H
#define FLINT_UI_H

#define CLR_RESET   "\033[0m"
#define CLR_BOLD    "\033[1m"
#define CLR_RED     "\033[31m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_BLUE    "\033[34m"
#define CLR_CYAN    "\033[36m"
#define CLR_GRAY    "\033[90m"

#endif /* FLINT_UI_H */
