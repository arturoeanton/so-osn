/*
 * /bin/uxsh — micro-shell intended to be spawned inside oxterm.
 *
 * Distinct from /bin/shellsrv (the SYSTEM shell that owns
 * SERVER_SHELL): uxsh is just a line-editor + fork/execve helper,
 * one per terminal window, no service registration. Lets the user
 * type Unix-style commands inside an Ox terminal:
 *
 *   $ ls /home
 *   $ cat /home/.oxrc
 *   $ ovi /home/notepad.txt
 *
 * Supported syntax (intentionally minimal — KISS):
 *   - Space-separated argv (no quoting, no globbing).
 *   - Bare relative names looked up in /bin/.
 *   - Absolute paths used as-is.
 *   - Built-ins: cd <dir>, exit, pwd, help, clear.
 *   - No pipes, no redirection, no &. Use shellsrv for those.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MAX_ARGS 16
#define LINE_MAX_LEN 256

static char g_cwd[256] = "/";

static int parse_line(char *line, char *argv[MAX_ARGS]) {
    int argc = 0;
    char *p = line;
    while (argc < MAX_ARGS - 1 && *p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    argv[argc] = NULL;
    return argc;
}

static int run_builtin(int argc, char *argv[]) {
    if (strcmp(argv[0], "exit") == 0) {
        _exit(0);
    }
    if (strcmp(argv[0], "pwd") == 0) {
        if (getcwd(g_cwd, sizeof(g_cwd))) printf("%s\n", g_cwd);
        return 1;
    }
    if (strcmp(argv[0], "cd") == 0) {
        const char *target = (argc > 1) ? argv[1] : "/home";
        if (chdir(target) < 0) {
            printf("cd: %s: errno=%d\n", target, errno);
        } else {
            getcwd(g_cwd, sizeof(g_cwd));
        }
        return 1;
    }
    if (strcmp(argv[0], "clear") == 0) {
        fputs("\x1b[2J\x1b[H", stdout);
        fflush(stdout);
        return 1;
    }
    if (strcmp(argv[0], "help") == 0) {
        printf("uxsh — micro shell. built-ins: cd pwd clear help exit\n");
        printf("anything else: /bin/<name> args... (fork+exec)\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    getcwd(g_cwd, sizeof(g_cwd));
    printf("uxsh — type 'help' for built-ins, 'exit' to close.\n");
    char line[LINE_MAX_LEN];
    for (;;) {
        printf("%s$ ", g_cwd);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            return 0;
        }
        size_t L = strlen(line);
        while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (L == 0) continue;

        char *args[MAX_ARGS];
        int ac = parse_line(line, args);
        if (ac == 0) continue;

        if (run_builtin(ac, args)) continue;

        /* Resolve binary path: absolute used as-is; bare names looked
         * up in /bin/. */
        char path[128];
        if (args[0][0] == '/') {
            strncpy(path, args[0], sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
        } else {
            snprintf(path, sizeof(path), "/bin/%s", args[0]);
        }

        pid_t pid = fork();
        if (pid < 0) {
            printf("fork failed: errno=%d\n", errno);
            continue;
        }
        if (pid == 0) {
            execve(path, args, environ);
            fprintf(stderr, "uxsh: %s: not found (errno=%d)\n", path, errno);
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            printf("\n[killed by signal %d]\n", WTERMSIG(status));
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            printf("[exit %d]\n", WEXITSTATUS(status));
        }
    }
}
