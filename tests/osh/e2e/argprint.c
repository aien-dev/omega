/*
 * argprint.c -- tiny helper program for the osh end-to-end tests (aien-architecture#158). Not part of osh.
 *   argprint ARGS...      prints each argument as [arg] on its own line
 *   argprint -e NAME      prints [value] of an environment variable, or [(unset)]
 *   argprint -x N         exits with status N, printing nothing
 *   argprint -cat         copies standard input to standard output
 *   argprint -io          writes "O" to standard output and "E" to standard error, in that order
 *   argprint -pwd         prints the current directory
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "-e") == 0) {
        const char *v = getenv(argv[2]);
        printf("[%s]\n", v ? v : "(unset)");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "-x") == 0) return atoi(argv[2]);
    if (argc == 2 && strcmp(argv[1], "-cat") == 0) {
        char b[4096];
        ssize_t r;
        while ((r = read(0, b, sizeof b)) > 0)
            if (write(1, b, (size_t)r) != r) return 1;
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "-io") == 0) {
        if (write(1, "O", 1) != 1) return 1;
        if (write(2, "E", 1) != 1) return 1;
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "-pwd") == 0) {
        char b[4096];
        if (!getcwd(b, sizeof b)) return 1;
        printf("[%s]\n", b);
        return 0;
    }
    for (int i = 1; i < argc; i++) printf("[%s]\n", argv[i]);
    return 0;
}
