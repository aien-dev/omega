#ifndef OMEGA_BLACKWELL_GATES_H
#define OMEGA_BLACKWELL_GATES_H

/* Runs all 18 qualification gates for Milestone 17. Returns 0 if all pass, 1 on any failure */
int run_m17_gates(void);

/* Runs physical silicon demonstration of vector addition on Grace Blackwell GB10 */
void run_demonstration_blackwell_vector(void);

/* Runs qualification gates for Milestone 18 (Stage 1). Returns 0 if Stage-1 passes, 1 on failure */
int run_m18_gates(void);

/* Runs physical silicon demonstration of matrix multiplication on Grace Blackwell GB10 */
void run_demonstration_blackwell_matmul(void);

#endif /* OMEGA_BLACKWELL_GATES_H */
