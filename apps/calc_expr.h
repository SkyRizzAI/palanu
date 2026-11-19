/* The calculator's expression evaluator (calc_expr.c): pure, so tests/unit/test_calc.c checks it. */
#ifndef PAL_APPS_CALC_EXPR_H
#define PAL_APPS_CALC_EXPR_H

#include <stdbool.h>

/* Evaluates expr (+ - * / % ^, parentheses, unary minus, 0x hex, pi, e, ans, and sqrt sin cos tan asin acos atan ln log exp abs
 * floor ceil round). ans is the value `ans` stands for. False with *err pointing at a reason when it cannot. */
bool calc_eval(const char *expr, double ans, double *out, const char **err);

#endif
