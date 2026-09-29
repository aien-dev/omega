#ifndef RX_RESIDENT_GPU_H
#define RX_RESIDENT_GPU_H

#include "rx_world.h"

/* One persistent graphics seat on the world's existing image.
 * A null out and a nonzero return means the chip was not started.
 * A non-null out must be finished, whether or not the seat was observed. */
typedef struct RxGpuSeat RxGpuSeat;

int rx_gpu_seat_begin(RxWorld *w, RxGpuSeat **out);
int rx_gpu_seat_finish(RxGpuSeat *seat);

#endif
