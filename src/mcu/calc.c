/* calc.c - see calc.h */
#include <stdint.h>
#include "calc.h"

#define CALC_MAX     999999999LL        /* largest number shown (9 digits)  */
#define MAX_DIGITS   9

enum { OP_NONE, OP_ADD, OP_SUB, OP_MUL };

static int64_t acc;                     /* result so far                                   */
static int     has_acc;                 /* acc holds a number (first one typed, or result) */
static int64_t entry;                   /* number being typed (without its sign)           */
static int     entry_digits, entry_neg;
static int     op;                      /* operator waiting for its second number          */
static int     after_eq;                /* last word was "equals"                          */
static int     err;
static char    disp[12], expr[24];

static char *put_num(char *p, int64_t v)
{
    char t[12];
    int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}

static char op_char(int o) { return o == OP_ADD ? '+' : o == OP_SUB ? '-' : 'X'; }

static int len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void update_display(void)
{
    char *p = disp;
    if (err) { char *s = "ERROR"; while (*s) *p++ = *s++; *p = 0; return; }
    if (entry_digits || entry_neg) {
        if (entry_neg) *p++ = '-';
        if (entry_digits) put_num(p, entry); else *p = 0;
    } else {
        put_num(p, has_acc ? acc : 0);
    }
}

void calc_reset(void)
{
    acc = 0; has_acc = 0;
    entry = 0; entry_digits = 0; entry_neg = 0;
    op = OP_NONE; after_eq = 0; err = 0;
    expr[0] = 0;
    update_display();
}

/* acc = acc (op) b; returns 0 on overflow */
static int apply(int64_t b)
{
    int64_t r = op == OP_ADD ? acc + b : op == OP_SUB ? acc - b : op == OP_MUL ? acc * b : b;
    if (r > CALC_MAX || r < -CALC_MAX) return 0;
    acc = r;
    return 1;
}

static void overflow(void)
{
    char *p = expr, *s = "TOO BIG";
    err = 1;
    while (*s) *p++ = *s++;
    *p = 0;
}

/* the number typed so far, with its sign */
static int64_t take_entry(void)
{
    int64_t v = entry_neg ? -entry : entry;
    entry = 0; entry_digits = 0; entry_neg = 0;
    return v;
}

/* "12 X" : what is waiting for its second number */
static void show_pending(void)
{
    char *p = put_num(expr, acc);
    *p++ = ' '; *p++ = op_char(op); *p = 0;
}

static void digit(int d)
{
    if (err || after_eq) calc_reset();          /* a digit after "equals" starts a new sum */
    if (entry_digits >= MAX_DIGITS) return;     /* ignore digits that do not fit */
    if (entry_digits == 1 && entry == 0) entry_digits = 0;   /* no leading zeros */
    entry = entry * 10 + d;
    entry_digits++;
}

static void operator(int o)
{
    if (err) return;
    after_eq = 0;
    if (entry_digits) {
        int64_t v = take_entry();
        if (op != OP_NONE && has_acc) { if (!apply(v)) { overflow(); return; } }
        else acc = v;
        has_acc = 1;
    } else if (!has_acc) {                      /* nothing typed yet */
        if (o == OP_SUB) entry_neg = !entry_neg;   /* "minus seven": a negative number */
        return;
    }
    op = o;                                     /* a second operator replaces the first */
    show_pending();
}

static void equals(void)
{
    if (err) return;
    if (entry_digits && op != OP_NONE && has_acc) {
        int64_t a = acc, b = take_entry();
        if (!apply(b)) { overflow(); return; }
        char *p = put_num(expr, a);             /* "12 X 3 =" */
        *p++ = ' '; *p++ = op_char(op); *p++ = ' ';
        p = put_num(p, b);
        *p++ = ' '; *p++ = '='; *p = 0;
        if (len(expr) > 21) {                   /* too long for the line: keep the end */
            p = expr; *p++ = op_char(op); *p++ = ' ';
            p = put_num(p, b); *p++ = ' '; *p++ = '='; *p = 0;
        }
    } else if (entry_digits) {                  /* just a number: it becomes the result */
        acc = take_entry();
        has_acc = 1;
        expr[0] = 0;
    } else {                                    /* nothing new typed: forget a waiting operator */
        entry_neg = 0;
        if (has_acc) { char *p = put_num(expr, acc); *p++ = ' '; *p++ = '='; *p = 0; }
    }
    op = OP_NONE;
    after_eq = 1;
}

int calc_word(int w)
{
    if (w >= 0 && w <= 9) digit(w);
    else if (w == CALC_PLUS) operator(OP_ADD);
    else if (w == CALC_MINUS) operator(OP_SUB);
    else if (w == CALC_MULTIPLY) operator(OP_MUL);
    else if (w == CALC_EQUALS) equals();
    else if (w == CALC_CLEAR) calc_reset();
    else return 0;
    update_display();
    return 1;
}

const char *calc_display(void) { return disp; }
const char *calc_expr(void)    { return expr; }
