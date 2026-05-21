/*
 * /bin/envtest — print the inherited environment + try getenv("PATH").
 *
 *   exec /bin/envtest [VAR]
 *
 * No args: dump every entry of `environ`.
 * One arg: print getenv(VAR) (or "(unset)").
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc >= 2) {
        const char *v = getenv(argv[1]);
        printf("%s=%s\n", argv[1], v ? v : "(unset)");
        return 0;
    }

    printf("envtest: dumping environ\n");
    if (!environ || !environ[0]) {
        printf("  (empty)\n");
        return 0;
    }
    for (int i = 0; environ[i]; i++) {
        printf("  %s\n", environ[i]);
    }

    /* And demonstrate setenv/unsetenv survive within this process. */
    setenv("OSNOS", "v0.0", 1);
    printf("\nafter setenv OSNOS=v0.0:\n  OSNOS=%s\n", getenv("OSNOS"));
    unsetenv("OSNOS");
    const char *gone = getenv("OSNOS");
    printf("after unsetenv:\n  OSNOS=%s\n", gone ? gone : "(unset)");
    return 0;
}
