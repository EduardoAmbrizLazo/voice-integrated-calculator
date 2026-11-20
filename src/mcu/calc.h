/* calc.h - the calculator: turns recognised words into arithmetic
 *
 * Works like a pocket calculator, one word at a time:
 *   "seven multiply six equals"      -> 42
 *   "one two plus three zero equals" -> 42   (digits build a number)
 *   "minus seven plus two equals"    -> -5   ("minus" first = negative number)
 *   "two plus three multiply four equals" -> 20  (left to right, like a pocket calculator)
 *   after "equals": a digit starts a new sum, an operator carries on from the result
 *   two operators in a row: the second one replaces the first
 *   "clear" starts over
 * Whole numbers only, up to 9 digits; a result past +-999,999,999 shows ERROR until the
 * next digit or "clear". The clock/timer words are not handled here (calc_word returns 0).
 *
 * No hardware in here, so it can be tested on a PC (tests/host/calc_test.c).
 */
#ifndef CALC_H
#define CALC_H

/* Word numbers, in the order of the vocabulary in enroll.c */
enum { CALC_PLUS = 10, CALC_MINUS = 11, CALC_MULTIPLY = 12, CALC_EQUALS = 13, CALC_CLEAR = 14 };

void        calc_reset(void);
int         calc_word(int w);       /* 0..9 = digit, or CALC_*; returns 1 if the calculator used it */
const char *calc_display(void);     /* the number to show big, e.g. "42", "-5", "ERROR" (<= 10 chars) */
const char *calc_expr(void);        /* the sum so far for the line above it, e.g. "7 X 6 =" (<= 21 chars) */

#endif
