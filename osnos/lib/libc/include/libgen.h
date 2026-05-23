#pragma once

/*
 * dirname() / basename() POSIX path manipulators. They MUTATE
 * their argument (insert NULs) — caller must pass a writable
 * copy if the original needs preserving.
 */

char *dirname (char *path);
char *basename(char *path);
