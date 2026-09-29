#include <string.h>

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--run-x-gates") == 0) return 0;
    return 1;
}
