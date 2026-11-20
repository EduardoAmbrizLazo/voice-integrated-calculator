/* Self-checking test of src/mcu/calc.c: feeds word sequences and checks what
 * the screen would show.  Exit status 1 if anything fails. */
#include <stdio.h>
#include <string.h>
#include "calc.h"

static int fails, total;

/* words: space separated, digits as 0-9, + - x = c for the operators and "clear" */
static void check(const char *words, const char *want_disp, const char *want_expr)
{
    calc_reset();
    for (const char *p = words; *p; p++) {
        int w = -1;
        if (*p >= '0' && *p <= '9') w = *p - '0';
        else if (*p == '+') w = CALC_PLUS;
        else if (*p == '-') w = CALC_MINUS;
        else if (*p == 'x') w = CALC_MULTIPLY;
        else if (*p == '=') w = CALC_EQUALS;
        else if (*p == 'c') w = CALC_CLEAR;
        if (w >= 0) calc_word(w);
    }
    int ok = !strcmp(calc_display(), want_disp) && (!want_expr || !strcmp(calc_expr(), want_expr));
    total++;
    if (!ok) fails++;
    printf("  %s  %-26s -> %-11s %-22s%s\n", ok ? "pass" : "FAIL", words, calc_display(), calc_expr(),
           ok ? "" : "  <- expected different");
}

int main(void)
{
    printf("calculator\n");
    check("7 x 6 =",            "42",    "7 X 6 =");
    check("1 2 + 3 0 =",        "42",    "12 + 30 =");
    check("5 - 9 =",            "-4",    "5 - 9 =");
    check("2 + 3 x 4 =",        "20",    "5 X 4 =");        /* left to right */
    check("- 7 + 2 =",          "-5",    "-7 + 2 =");       /* minus first = negative */
    check("7 x",                "7",     "7 X");            /* operator waiting */
    check("7 x 6",              "6",     "7 X");            /* typing the second number */
    check("4 + + 5 =",          "9",     "4 + 5 =");        /* repeated operator */
    check("4 + - 5 =",          "-1",    "4 - 5 =");        /* second operator replaces */
    check("1 + 2 = + 3 =",      "6",     "3 + 3 =");        /* carry on from a result */
    check("1 + 2 = 7",          "7",     "");               /* digit after = starts over */
    check("9 x =",              "9",     "9 =");            /* = with nothing after the operator */
    check("=",                  "0",     "");
    check("0 0 7",              "7",     "");               /* no leading zeros */
    check("1 2 3 4 5 6 7 8 9 0", "123456789", "");          /* 10th digit ignored */
    check("9 9 9 9 9 9 9 9 9 x 9 =", "ERROR", "TOO BIG");
    check("9 9 9 9 9 9 9 9 9 x 9 = 5", "5", "");            /* digit clears the error */
    check("9 9 9 9 9 9 9 9 9 + 1 x", "ERROR", "TOO BIG");   /* overflow while chaining */
    check("5 x 3 c",            "0",     "");               /* clear */
    check("5 c 2 + 2 =",        "4",     "2 + 2 =");
    check("0 - 1 2 3 4 5 6 7 8 9 =", "-123456789", "0 - 123456789 =");
    check("- 9 9 9 9 9 9 9 9 9 - 9 9 9 9 9 9 9 9 9 =", "ERROR", "TOO BIG");
    check("- 1 2 3 4 5 6 7 8 9 x 1 2 3 4 5 6 7 8 =", "ERROR", "TOO BIG");
    check("- 1 2 3 4 5 6 7 8 9 - 1 2 3 4 5 6 7 8 9 =", "-246913578", "- 123456789 =");   /* too long for the line: keeps the end */
    printf("\n%s: %d of %d calculator checks passed\n", fails ? "FAIL" : "PASS", total - fails, total);
    return fails ? 1 : 0;
}
