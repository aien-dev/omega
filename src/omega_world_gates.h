#ifndef OMEGA_WORLD_GATES_H
#define OMEGA_WORLD_GATES_H

/* Runs all 18 qualification gates for Milestone 19. Returns 0 if all pass, 1 on any failure */
int run_m19_gates(void);

/* Standalone silicon test (not a canonical gate): completion payloads are
 * deliberately decoupled from the GPFIFO put sequence. Returns 0 on pass. */
int run_m19_drain_decoupling(void);

/* M19R world lifecycle gates (memory release, quarantine, fault latch,
 * monotonic completion). Returns 0 if all pass. */
int run_world_lifecycle_gates(void);

/* Runs persistent accelerator world physical silicon demonstration */
void run_demonstration_accelerator_world(void);

#endif /* OMEGA_WORLD_GATES_H */
