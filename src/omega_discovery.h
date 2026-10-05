#ifndef OMEGA_DISCOVERY_H
#define OMEGA_DISCOVERY_H

#include "omega_program.h"
#include "omega_library.h"
#include "omega_synthesis.h"
#include <stdbool.h>
#include <stddef.h>

#define OMEGA_CORPUS_MAX_PROGRAMS 16
#define OMEGA_DISCOVERY_MAX_CANDIDATES 16
#define OMEGA_DISCOVERY_MAX_OCCURRENCES 16

typedef struct {
    OmegaProgram programs[OMEGA_CORPUS_MAX_PROGRAMS];
    size_t count;
} OmegaCorpus;

typedef struct {
    OmegaProgram abstraction;
    size_t occurrence_count;
    size_t occurrences[OMEGA_DISCOVERY_MAX_OCCURRENCES];
    uint32_t slice_offset_insns;
    uint32_t slice_len_insns;
    int32_t compression_score;   /* Net instructions saved across corpus */
    bool is_nontrivial;
    bool is_verified;
    VerifyReport verify_report;
} OmegaAbstractionCandidate;

typedef struct {
    OmegaAbstractionCandidate candidates[OMEGA_DISCOVERY_MAX_CANDIDATES];
    size_t candidate_count;
    int best_candidate_index;
} OmegaDiscoveryResult;

/* Initialize empty corpus */
void omega_corpus_init(OmegaCorpus *corpus);

/* Free resources in corpus */
void omega_corpus_destroy(OmegaCorpus *corpus);

/* Add a verified program to the corpus */
int omega_corpus_add(OmegaCorpus *corpus, const OmegaProgram *prog);

/* Compute total instruction cost of corpus */
uint32_t omega_corpus_total_cost(const OmegaCorpus *corpus);

/* Populate corpus with canonical benchmark programs demonstrating recurring sub-expression (2x + 1) */
int omega_corpus_populate_benchmark(OmegaCorpus *corpus);

/* Mine corpus for repeating common sub-expression slices and evaluate compression */
int omega_discover_abstractions(const OmegaCorpus *corpus, OmegaDiscoveryResult *result);

/* Refactor a program by substituting an abstraction for a common sub-expression */
int omega_refactor_program(const OmegaProgram *orig, const OmegaProgram *abstraction,
                           uint32_t match_offset_insns, uint32_t match_len_insns,
                           OmegaProgram *out_refactored);

/* Verify semantic preservation: compare original and refactored programs on test inputs */
bool omega_verify_semantic_preservation(const OmegaProgram *orig, const OmegaProgram *refactored,
                                        const uint64_t *test_inputs, size_t input_count);

/* Admitting a discovered abstraction into a library goes through omega_vc_bridge_admit_abstraction
 * (src/omega_vc_bridge.h): the Verified Crumb Store admit path with a receipt (VC1 stage 6). */

/* Demonstrate search acceleration on held-out task */
int omega_demonstrate_search_acceleration(const OmegaProgram *discovered_abstraction,
                                          size_t *out_candidates_without,
                                          size_t *out_candidates_with);

#endif /* OMEGA_DISCOVERY_H */
