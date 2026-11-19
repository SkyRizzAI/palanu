/* calc [EXPRESSION]: evaluates one expression, or with none reads one per line (empty line or Ctrl-D ends); `ans` is the last
 * result. See calc_expr.h for what an expression may hold. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "calc_expr.h"
#include "libapp.h"

static void show(double v) {
    char t[48];
    if (v == (double)(long long)v && v < 1e15 && v > -1e15) snprintf(t, sizeof(t), "%lld", (long long)v);
    else snprintf(t, sizeof(t), "%.10g", v);
    pal_printf("%s\n", t);
}

static int cmd_calc(int argc, char **argv) {
    double ans = 0;
    const char *err = NULL;
    if (argc > 1) {
        char e[256] = "";
        for (int i = 1; i < argc; i++) { /* calc 2 + 3 and calc '2 + 3' are the same */
            strncat(e, argv[i], sizeof(e) - strlen(e) - 2);
            strncat(e, " ", sizeof(e) - strlen(e) - 1);
        }
        if (!calc_eval(e, ans, &ans, &err)) {
            pal_eprintf("calc: %s\n", err);
            return 1;
        }
        show(ans);
        return 0;
    }
    pal_printf("calc: one expression per line; ans is the last result; an empty line ends\n");
    char line[160];
    for (;;) {
        pal_printf("> ");
        int n = nema_read(0, line, sizeof(line) - 1);
        if (n <= 0) break;
        line[n] = 0;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) break;
        double v;
        if (calc_eval(line, ans, &v, &err)) show(ans = v);
        else pal_printf("  %s\n", err);
    }
    return 0;
}

static const nema_app_t CALC_APPS[] = {{"calc", "calculator: calc 2*(3+4), sqrt(2), 0xff + 1; no argument: line by line", cmd_calc, 6144, 0}};

void pal_apps_register_calc(void);
void pal_apps_register_calc(void) { nema_app_register(&CALC_APPS[0]); }
