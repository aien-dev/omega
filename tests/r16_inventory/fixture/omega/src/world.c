/* R16 inventory fixture. while (1) { } in a comment is not a site. */
static const char *s = "run_until_complete while(1) for(;;)";
int pop(void);
int ready(void);
int usleep(unsigned);

int worker_main(void) {
    for (;;) {
        if (pop()) break;
    }
    return 0;
}

int data_loop(int n) {
    int t = 0;
    for (int i = 0; i < n; i++) t += i; /* bounded data loop: not a site */
    while (t > 100) t -= 7;               /* no wait word; "return" is not "turn" */
    return t + (s != 0);
}

int wait_ready(void) {
    for (int i = 0; i < 100; i++) {
        if (ready()) return 0;
        usleep(1000);
    }
    return -1;
}
