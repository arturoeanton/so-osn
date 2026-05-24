/*
 * unixtest — smoke test de AF_UNIX SOCK_STREAM.
 *
 * Server bind+listen+accept; cliente forkeado connect+send+recv. Un
 * round trip "PING"→"PONG" prueba el path completo.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SOCK_PATH "/tmp/osnos-unix-test"

static int server_loop(int srv) {
    /* Accept retry: AF_UNIX accept es non-blocking single-shot,
     * retorna EAGAIN si nada pendiente. */
    int cli = -1;
    for (int i = 0; i < 200; i++) {
        cli = accept(srv, 0, 0);
        if (cli >= 0) break;
        if (errno != EAGAIN) {
            fprintf(stderr, "accept failed: errno=%d\n", errno);
            return 1;
        }
        usleep(10000);    /* 10 ms */
    }
    if (cli < 0) { fprintf(stderr, "accept timed out\n"); return 1; }

    /* Receive PING. */
    char buf[64];
    int n = -1;
    for (int i = 0; i < 200; i++) {
        n = read(cli, buf, sizeof(buf) - 1);
        if (n > 0) break;
        if (n == 0) {
            fprintf(stderr, "server: client closed early\n");
            close(cli); return 1;
        }
        if (errno != EAGAIN) {
            fprintf(stderr, "server read failed: errno=%d\n", errno);
            close(cli); return 1;
        }
        usleep(10000);
    }
    if (n <= 0) { fprintf(stderr, "server: no data\n"); close(cli); return 1; }
    buf[n] = 0;
    printf("server got: '%s'\n", buf);

    /* Reply PONG. */
    const char *reply = "PONG";
    int w = write(cli, reply, 4);
    if (w != 4) {
        fprintf(stderr, "server write failed: w=%d errno=%d\n", w, errno);
        close(cli); return 1;
    }

    close(cli);
    return 0;
}

static int client_round(void) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { fprintf(stderr, "client socket failed: errno=%d\n", errno); return 1; }

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    /* Connect retry: el server puede no estar listo todavía. */
    int rc = -1;
    for (int i = 0; i < 200; i++) {
        rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
        if (rc == 0) break;
        if (errno != ENOENT && errno != ECONNREFUSED) {
            fprintf(stderr, "connect failed: errno=%d\n", errno);
            close(s); return 1;
        }
        usleep(10000);
    }
    if (rc != 0) { fprintf(stderr, "connect timed out\n"); close(s); return 1; }

    /* Send PING. */
    const char *msg = "PING";
    int w = write(s, msg, 4);
    if (w != 4) {
        fprintf(stderr, "client write failed: w=%d errno=%d\n", w, errno);
        close(s); return 1;
    }

    /* Read PONG. */
    char buf[64];
    int n = -1;
    for (int i = 0; i < 200; i++) {
        n = read(s, buf, sizeof(buf) - 1);
        if (n > 0) break;
        if (n == 0) {
            fprintf(stderr, "client: server closed early\n");
            close(s); return 1;
        }
        if (errno != EAGAIN) {
            fprintf(stderr, "client read failed: errno=%d\n", errno);
            close(s); return 1;
        }
        usleep(10000);
    }
    if (n <= 0) { close(s); return 1; }
    buf[n] = 0;
    printf("client got: '%s'\n", buf);

    close(s);
    return (strcmp(buf, "PONG") == 0) ? 0 : 1;
}

int main(void) {
    /* Server side: socket, bind, listen ANTES de fork. Sino el cliente
     * podría tratar de connect antes que exista el path. */
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        fprintf(stderr, "server socket failed: errno=%d\n", errno);
        return 1;
    }

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind failed: errno=%d\n", errno);
        close(srv); return 1;
    }
    if (listen(srv, 4) != 0) {
        fprintf(stderr, "listen failed: errno=%d\n", errno);
        close(srv); return 1;
    }

    int pid = fork();
    if (pid < 0) { fprintf(stderr, "fork failed\n"); close(srv); return 1; }
    if (pid == 0) {
        /* Child = client. */
        close(srv);
        return client_round();
    }

    /* Parent = server. */
    int srv_rc = server_loop(srv);
    close(srv);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    int cli_rc = (wstatus >> 8) & 0xff;

    if (srv_rc == 0 && cli_rc == 0) {
        printf("unixtest: OK\n");
        return 0;
    }
    fprintf(stderr, "unixtest: FAIL (srv=%d cli=%d)\n", srv_rc, cli_rc);
    return 1;
}
