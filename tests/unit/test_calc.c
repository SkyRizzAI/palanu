/* The calculator's evaluator (apps/calc_expr.c): precedence, functions, constants, hex, ans, and the errors it reports. */
#include <math.h>

#include "../../apps/calc_expr.h"
#include "test.h"

static double ev(const char *e) {
    double v = NAN;
    const char *err;
    return calc_eval(e, 10, &v, &err) ? v : NAN;
}

TEST(precedence_and_associativity) {
    CHECK(ev("2+3*4") == 14);
    CHECK(ev("(2+3)*4") == 20);
    CHECK(ev("2^3^2") == 512);  /* right to left */
    CHECK(ev("-2^2") == -4);    /* the power first */
    CHECK(ev("2^-1") == 0.5);
    CHECK(ev("7 % 3") == 1);
    CHECK(ev("10 - 4 - 3") == 3);
}

TEST(names_functions_and_hex) {
    CHECK(fabs(ev("sqrt(2)") - 1.41421356237) < 1e-9);
    CHECK(fabs(ev("sin(pi/2)") - 1) < 1e-12);
    CHECK(ev("0xff + 1") == 256);
    CHECK(ev("ans * 2") == 20);
    CHECK(fabs(ev("ln(e)") - 1) < 1e-12);
    CHECK(ev("log(1000)") == 3);
}

TEST(errors_are_reported) {
    double v;
    const char *err = NULL;
    CHECK(!calc_eval("1/0", 0, &v, &err) && err);
    CHECK(!calc_eval("(1+2", 0, &v, &err));
    CHECK(!calc_eval("foo(1)", 0, &v, &err));
    CHECK(!calc_eval("2 +", 0, &v, &err));
    CHECK(!calc_eval("2 3", 0, &v, &err));
}

int main(void) {
    RUN(precedence_and_associativity);
    RUN(names_functions_and_hex);
    RUN(errors_are_reported);
    return TEST_DONE();
}
