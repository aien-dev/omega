#ifndef RX_RESIDENT_GPU_H
#define RX_RESIDENT_GPU_H

#include "rx_world.h"

/* Words in the seat's heartbeat block, as byte offsets from its start. */
#define RX_SEAT_HB_HOLD  8u    /* processor writes nonzero: seat holds its claim */
#define RX_SEAT_HB_HELD  12u   /* seat writes claim sequence + 1 before the hold */
#define RX_SEAT_HB_LIVE  16u   /* seat writes a moving count on every pass */

/* One persistent graphics seat on the world's existing image.
 * A null out and a nonzero return means the chip was not started.
 * A non-null out must be finished, whether or not the seat was observed. */
typedef struct RxGpuSeat RxGpuSeat;

int rx_gpu_seat_begin(RxWorld *w, RxGpuSeat **out);
int rx_gpu_seat_finish(RxGpuSeat *seat);

/* Destroy the seat's channel while it runs, as a fault would. When this
 * returns the chip no longer writes the image; the image stays mapped and
 * bound. The caller then declares the seat lost to the world. */
int rx_gpu_seat_kill(RxGpuSeat *seat);

/* After a kill: build a new channel on the same device and memory and launch
 * a new seat on the same image, taking the world's current seat generation. */
int rx_gpu_seat_relaunch(RxGpuSeat *seat);

#endif
