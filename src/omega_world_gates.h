#ifndef OMEGA_WORLD_GATES_H
#define OMEGA_WORLD_GATES_H

/* Runs all 17 qualification gates for Milestone 19. Returns 0 if all pass, 1 on any failure */
int run_m19_gates(void);
int run_m19_receipt_only(void);

/* Runs persistent accelerator world physical silicon demonstration */
void run_demonstration_accelerator_world(void);

#endif /* OMEGA_WORLD_GATES_H */
