#include "omega_numeric_native.h"
int numeric_other_launcher_open(void) {
    M16NativeContext ctx;
    return omega_numeric_native_open(&ctx);
}
