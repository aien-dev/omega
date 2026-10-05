/* rx_operator: the operator's client for a running production world
 * (docs/r16-operator-control.md). It reads the operator credential file,
 * sends one request to the program's owner-only socket and prints the one
 * reply line. It decides nothing: the world authority does.
 *
 *   rx_operator --control <state>/control <command> [reason]
 *   rx_operator --cred <file> <command> [reason]      present another credential file
 *   rx_operator --control <dir> --raw '<line>'        send a line as is (tests)
 *
 * <command>: status | stop | resume | revoke-cap | revoke | shutdown
 * Exit: 0 reply OK, 1 any other reply, 2 usage, no credential, or no world. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int usage(void) {
    fprintf(stderr, "usage: rx_operator (--control <dir> | --cred <file>) "
                    "(status|stop [reason]|resume|revoke-cap|revoke|shutdown | --raw <line>)\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *control = NULL, *cred_file = NULL, *raw = NULL, *cmd = NULL, *reason = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--control") == 0 && i + 1 < argc) control = argv[++i];
        else if (strcmp(argv[i], "--cred") == 0 && i + 1 < argc) cred_file = argv[++i];
        else if (strcmp(argv[i], "--raw") == 0 && i + 1 < argc) raw = argv[++i];
        else if (!cmd) cmd = argv[i];
        else if (!reason) reason = argv[i];
        else return usage();
    }
    if ((!control && !cred_file) || (!raw && !cmd) || (raw && cmd)) return usage();
    char path[512];
    if (cred_file) snprintf(path, sizeof path, "%s", cred_file);
    else snprintf(path, sizeof path, "%s/operator.cred", control);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "rx_operator: no credential at %s: %s\n", path, strerror(errno)); return 2; }
    char line[256], sock[256] = "", secret[80] = "";
    unsigned subject = 0, cap_id = 0;
    unsigned long long gen = 0, cap_gen = 0;
    int have = 0;
    if (!fgets(line, sizeof line, f) || strcmp(line, "aien-operator-credential v1\n") != 0) {
        fclose(f);
        fprintf(stderr, "rx_operator: %s is not an operator credential file\n", path);
        return 2;
    }
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "socket %255s", sock) == 1) have |= 1;
        else if (sscanf(line, "subject %u", &subject) == 1) have |= 2;
        else if (sscanf(line, "cred %llu %79s", &gen, secret) == 2) have |= 4;
        else if (sscanf(line, "cap %u %llu", &cap_id, &cap_gen) == 2) have |= 8;
    }
    fclose(f);
    if (have != 15) { fprintf(stderr, "rx_operator: %s is incomplete\n", path); return 2; }
    if (control) snprintf(sock, sizeof sock, "%s/operator.sock", control);
    char req[1024];
    int n;
    if (raw) n = snprintf(req, sizeof req, "%s\n", raw);
    else if (reason)
        n = snprintf(req, sizeof req, "aien-operator v1 %s subject=%u gen=%llu secret=%s cap=%u:%llu "
                     "reason=%s\n", cmd, subject, gen, secret, cap_id, cap_gen, reason);
    else
        n = snprintf(req, sizeof req, "aien-operator v1 %s subject=%u gen=%llu secret=%s cap=%u:%llu\n",
                     cmd, subject, gen, secret, cap_id, cap_gen);
    memset(secret, 0, sizeof secret);
    if (n <= 0 || (size_t)n >= sizeof req) return usage();
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(sock) >= sizeof a.sun_path) { fprintf(stderr, "rx_operator: socket path too long\n"); return 2; }
    memcpy(a.sun_path, sock, strlen(sock) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "rx_operator: no world is serving %s: %s\n", sock, strerror(errno));
        return 2;
    }
    /* The program answers a request once the world it is setting up is ready. */
    struct timeval tv = {60, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (int off = 0; off < n;) {
        ssize_t w = send(fd, req + off, (size_t)(n - off), MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { fprintf(stderr, "rx_operator: send: %s\n", strerror(errno)); return 2; }
        off += (int)w;
    }
    memset(req, 0, sizeof req);
    shutdown(fd, SHUT_WR);
    char reply[2048];
    size_t got = 0;
    while (got < sizeof reply - 1) {
        ssize_t r = read(fd, reply + got, sizeof reply - 1 - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    reply[got] = 0;
    if (!got) { fprintf(stderr, "rx_operator: no reply\n"); return 2; }
    fputs(reply, stdout);
    if (reply[got - 1] != '\n') fputc('\n', stdout);
    return strncmp(reply, "aien-operator v1 OK", 19) == 0 ? 0 : 1;
}
