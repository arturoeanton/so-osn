/*
 * shmtest — smoke test de POSIX shm + mmap MAP_SHARED.
 *
 * Parent: shm_open("/test") + ftruncate(4096) + mmap(SHARED) +
 *   write "HELLO FROM PARENT" en byte 0, "PARENT_OK" en byte 100.
 * Fork. Child: mmap del mismo fd (heredado por fork) y verifica
 *   ver "HELLO FROM PARENT". Child sobrescribe byte 0 con
 *   "CHILD WAS HERE" y exits.
 * Parent waitpid, lee de nuevo y verifica que aparece la escritura
 *   del child.
 *
 * Si fork no preserva las shared pages (bug fixup en sys_fork),
 * el child vería una COPIA y el parent NO vería la escritura del
 * child. Este test catchea ese caso explícitamente.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHM_NAME "shmtest1"
#define SHM_SIZE 4096

int main(void) {
    /* O_CREAT|O_RDWR = 0x42 (Linux numbers). */
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { fprintf(stderr, "shm_open failed: errno=%d\n", errno); return 1; }

    if (ftruncate(fd, SHM_SIZE) != 0) {
        fprintf(stderr, "ftruncate failed: errno=%d\n", errno); return 1;
    }

    char *shm = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        fprintf(stderr, "mmap failed: errno=%d\n", errno); return 1;
    }

    /* Initialize. */
    strcpy(shm,             "HELLO FROM PARENT");
    strcpy(shm + 100,       "PARENT_OK");

    int pid = fork();
    if (pid < 0) { fprintf(stderr, "fork failed\n"); return 1; }

    if (pid == 0) {
        /* Child — same mapping inherited. Verifica ver lo que parent escribió. */
        if (strcmp(shm, "HELLO FROM PARENT") != 0) {
            fprintf(stderr, "child saw '%s' (expected 'HELLO FROM PARENT')\n", shm);
            _exit(1);
        }
        if (strcmp(shm + 100, "PARENT_OK") != 0) {
            fprintf(stderr, "child saw [+100]='%s' (expected 'PARENT_OK')\n", shm + 100);
            _exit(1);
        }
        printf("child: read parent's data OK\n");
        /* Sobreescribir para que el parent lo vea. */
        strcpy(shm, "CHILD WAS HERE");
        _exit(0);
    }

    /* Parent. */
    int wst = 0;
    waitpid(pid, &wst, 0);
    int crc = (wst >> 8) & 0xff;
    if (crc != 0) {
        fprintf(stderr, "child exited with %d\n", crc);
        return 1;
    }

    /* Verificar la escritura del child. */
    if (strcmp(shm, "CHILD WAS HERE") != 0) {
        fprintf(stderr, "parent saw '%s' after child wrote 'CHILD WAS HERE'\n", shm);
        return 1;
    }
    printf("parent: saw child's write OK ('%s')\n", shm);

    /* Cleanup. */
    munmap(shm, SHM_SIZE);
    close(fd);
    shm_unlink(SHM_NAME);

    printf("shmtest: OK\n");
    return 0;
}
