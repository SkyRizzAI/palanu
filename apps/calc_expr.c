/* A recursive-descent evaluator for calc (calc_expr.h). Precedence from low to high: + -, * / %, unary minus, ^ (right to left),
 * then numbers, names and parentheses. */
#include "calc_expr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    double ans;
    const char *err;
} st_t;

static double expr(st_t *s);

static void ws(st_t *s) {
    while (*s->p == ' ' || *s->p == '\t') s->p++;
}

static bool name_is(const char *a, size_t n, const char *b) { return strlen(b) == n && !strncmp(a, b, n); }

static double primary(st_t *s) {
    ws(s);
    if (*s->p == '(') {
        s->p++;
        double v = expr(s);
        ws(s);
        if (*s->p != ')') s->err = s->err ? s->err : "a ( is not closed";
        else s->p++;
        return v;
    }
    if ((*s->p >= '0' && *s->p <= '9') || *s->p == '.') {
        char *end;
        double v = (s->p[0] == '0' && (s->p[1] | 32) == 'x') ? (double)strtoull(s->p, &end, 16) : strtod(s->p, &end);
        if (end == s->p) s->err = "a number is not right";
        s->p = end;
        return v;
    }
    if ((*s->p | 32) >= 'a' && (*s->p | 32) <= 'z') {
        const char *a = s->p;
        while (((*s->p | 32) >= 'a' && (*s->p | 32) <= 'z') || (*s->p >= '0' && *s->p <= '9')) s->p++;
        size_t n = (size_t)(s->p - a);
        if (name_is(a, n, "pi")) return M_PI;
        if (name_is(a, n, "e")) return M_E;
        if (name_is(a, n, "ans")) return s->ans;
        static const struct {
            const char *n;
            double (*f)(double);
        } F[] = {{"sqrt", sqrt}, {"sin", sin}, {"cos", cos}, {"tan", tan}, {"asin", asin}, {"acos", acos}, {"atan", atan},
                 {"ln", log}, {"log", log10}, {"exp", exp}, {"abs", fabs}, {"floor", floor}, {"ceil", ceil}, {"round", round}};
        for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++)
            if (name_is(a, n, F[i].n)) {
                ws(s);
                if (*s->p != '(') {
                    s->err = "a function needs ( )";
                    return 0;
                }
                return F[i].f(primary(s));
            }
        s->err = "an unknown name";
        return 0;
    }
    s->err = *s->p ? "something unexpected" : "the expression ends too early";
    return 0;
}

static double power(st_t *s) {
    double b = primary(s);
    ws(s);
    if (*s->p == '^') {
        s->p++;
        double x = 0;
        ws(s);
        if (*s->p == '-') { /* 2^-1 */
            s->p++;
            x = -power(s);
        } else {
            x = power(s);
        }
        return pow(b, x);
    }
    return b;
}

static double unary(st_t *s) {
    ws(s);
    if (*s->p == '-') {
        s->p++;
        return -unary(s);
    }
    if (*s->p == '+') {
        s->p++;
        return unary(s);
    }
    return power(s);
}

static double term(st_t *s) {
    double v = unary(s);
    for (;;) {
        ws(s);
        char op = *s->p;
        if (op != '*' && op != '/' && op != '%') return v;
        s->p++;
        double r = unary(s);
        if (op == '*') v *= r;
        else if (r == 0) s->err = s->err ? s->err : "division by zero";
        else v = op == '/' ? v / r : fmod(v, r);
    }
}

static double expr(st_t *s) {
    double v = term(s);
    for (;;) {
        ws(s);
        char op = *s->p;
        if (op != '+' && op != '-') return v;
        s->p++;
        double r = term(s);
        v = op == '+' ? v + r : v - r;
    }
}

bool calc_eval(const char *text, double ans, double *out, const char **err) {
    st_t s = {text ? text : "", ans, NULL};
    double v = expr(&s);
    ws(&s);
    if (!s.err && *s.p) s.err = "something unexpected at the end";
    if (s.err) {
        if (err) *err = s.err;
        return false;
    }
    *out = v;
    return true;
}
