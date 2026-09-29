int usleep(unsigned);

int wait_marker(volatile int *m) {
    while (*m == 0) {
        usleep(10);
    }
    return 0;
}
