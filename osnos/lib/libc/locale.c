#include <locale.h>

/*
 * setlocale: osnos is C-locale always. Any non-NULL `locale` arg
 * that's not "C" or "" is rejected by returning NULL. The query
 * form (locale==NULL) returns the current locale name ("C").
 */
char *setlocale(int category, const char *locale) {
    (void)category;
    static char curr[] = "C";
    if (!locale) return curr;
    if (locale[0] == 0)                          return curr;   /* ""  */
    if (locale[0] == 'C' && locale[1] == 0)      return curr;   /* "C" */
    return 0;       /* unrecognized */
}

struct lconv *localeconv(void) {
    static char  dot[]   = ".";
    static char  empty[] = "";
    static struct lconv lc = {
        .decimal_point = dot,
        .thousands_sep = empty,
        .grouping      = empty,
    };
    return &lc;
}
