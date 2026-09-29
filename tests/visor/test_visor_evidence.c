/* Lane 4: visor_evidence tests (fixtures + read-only pass over the real evidence/ tree).
 * Run from the repo root. Prints "PASS n/n" on success. */
#include <stdio.h>
#include <string.h>

#include "omega_canonical.h"
#include "visor_evidence.h"

static int g_total, g_pass;
#define CHECK(cond, msg) do { g_total++; if (cond) g_pass++; else fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } while (0)

#define FIX "tests/visor/fixtures/evidence"

static VisorEvidenceView v, v2;
static char txt_a[65536], txt_b[65536], js[65536];

static const VisorEvidenceRecord *find(const VisorEvidenceView *view, const char *suffix) {
    size_t L = strlen(suffix);
    for (size_t i = 0; i < view->count; ++i) {
        size_t P = strlen(view->items[i].path);
        if (P >= L && strcmp(view->items[i].path + P - L, suffix) == 0) return &view->items[i];
    }
    return NULL;
}

int main(void) {
    /* --- scan fixtures --- */
    CHECK(visor_evidence_scan(FIX, &v) == 0, "scan fixtures rc");
    CHECK(v.scanned == 8 && v.count == 6 && v.matched == 6 && v.unparsed == 2 && !v.truncated, "scan counts");
    bool sorted = true;
    for (size_t i = 1; i < v.count; ++i) if (strcmp(v.items[i - 1].path, v.items[i].path) >= 0) sorted = false;
    CHECK(sorted, "sorted by path");
    CHECK(v.unparsed_listed == 2 && strstr(v.unparsed_paths[0], "digests.txt") &&
          strstr(v.unparsed_paths[1], "malformed_receipt.json"), "unparsed paths listed");

    const VisorEvidenceRecord *h = find(&v, "HOST/host_run_receipt.json");
    CHECK(h && h->scope == VISOR_QUAL_HOST && strcmp(h->scope_name, "HOST") == 0 &&
          strcmp(h->scope_basis, "hardware_scope") == 0, "host scope");
    CHECK(h && strcmp(h->commit, "1111111111111111111111111111111111111111") == 0 &&
          strcmp(h->run_id, "20260928T000000Z-fixturehost") == 0, "host commit/run");
    CHECK(h && h->tree_state_known && !h->tree_dirty && h->gates_run == 3 && h->gates_passed == 2, "host tree/gates");
    CHECK(h && strcmp(h->claim, "FIXTURE_HOST_RUN_V1: FIXTURE_HOST_PASS=PASS") == 0, "host claim");
    CHECK(h && strcmp(h->machine, "os=Linux arch=aarch64 kernel=7.0.0-fixture") == 0, "host machine");
    CHECK(h && strncmp(h->receipt_id, "sha256:", 7) == 0 && strlen(h->receipt_id) == 71 &&
          h->name_hash_match == -1, "receipt id");

    const VisorEvidenceRecord *q = find(&v, "qemu_milestone_receipt.json");
    CHECK(q && q->scope == VISOR_QUAL_QEMU && q->gates_run == 3 && q->gates_passed == 3, "qemu scope/gates");
    CHECK(q && strcmp(q->realization_id, "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0 &&
          !q->tree_state_known && strcmp(q->claim, "FIXTURE MILESTONE - QEMU: QUALIFIED / PASS") == 0, "qemu realization/claim");

    const VisorEvidenceRecord *s = find(&v, "SILICON/silicon_run_receipt.json");
    CHECK(s && s->scope == VISOR_QUAL_SILICON && strcmp(s->scope_basis, "silicon_observed=true") == 0, "silicon scope");
    CHECK(s && s->tree_dirty && strcmp(s->physics_commit, "5555555555555555555555555555555555555555") == 0 &&
          strcmp(s->machine, "node=spark-fixture arch=aarch64 kernel=7.0.0-fixture") == 0 &&
          strncmp(s->realization_id, "sha256:cccc", 11) == 0, "silicon fields");

    const VisorEvidenceRecord *n = find(&v, "SILICON/standin_no_upgrade_receipt.json");
    CHECK(n && n->scope == VISOR_QUAL_HOST, "silicon_observed=false + gpu_uuid is NOT upgraded");

    const VisorEvidenceRecord *u = find(&v, "unknown_scope_receipt.json");
    CHECK(u && u->scope == VISOR_QUAL_UNKNOWN && u->scope_basis[0] == 0 && u->gates_run == 0,
          "host{} + gpu_architecture + prose -> UNKNOWN");

    const VisorEvidenceRecord *c = NULL;
    for (size_t i = 0; i < v.count; ++i) if (strstr(v.items[i].path, "/CA/")) c = &v.items[i];
    CHECK(c && c->name_hash_match == 1 && c->scope == VISOR_QUAL_HOST &&
          strcmp(c->commit, "8888888888888888888888888888888888888888") == 0, "content-addressed name verified");

    /* --- load fails closed --- */
    VisorEvidenceRecord r;
    CHECK(visor_evidence_load(FIX "/malformed_receipt.json", &r) == -1, "malformed fails closed");
    CHECK(visor_evidence_load(FIX "/digests.txt", &r) == -1, "non-JSON fails closed");
    CHECK(visor_evidence_load(FIX "/does_not_exist.json", &r) == -1, "missing file fails closed");
    CHECK(visor_evidence_load(FIX "/SILICON/silicon_run_receipt.json", &r) == 0 && r.scope == VISOR_QUAL_SILICON,
          "load single");
    CHECK(visor_evidence_scan("tests/visor/fixtures/no_such_dir", &v2) == -1, "missing root is an error");

    /* --- for_id --- */
    SemanticId aid, missing;
    memset(aid.bytes, 0xaa, sizeof(aid.bytes));
    CHECK(visor_evidence_for_id(FIX, &aid, &v2) == 0, "for_id rc");
    CHECK(v2.count == 2 && find(&v2, "HOST/host_run_receipt.json") && find(&v2, "qemu_milestone_receipt.json") &&
          v2.unparsed == 2, "for_id finds mentions; unparsed mentions listed");
    SemanticId rid;
    memset(rid.bytes, 0xbb, sizeof(rid.bytes));
    visor_evidence_for_id(FIX, &rid, &v2);
    CHECK(v2.count == 1 && find(&v2, "qemu_milestone_receipt.json"), "for_id by realization id");
    memset(missing.bytes, 0x12, sizeof(missing.bytes));
    CHECK(visor_evidence_for_id(FIX, &missing, &v2) == 0 && v2.count == 0 && v2.unparsed == 0, "missing id -> count 0");
    CHECK(visor_evidence_format_text(&v2, txt_a, sizeof(txt_a)) > 0 && strstr(txt_a, "no evidence"), "no evidence text");

    /* --- rendering deterministic --- */
    visor_evidence_scan(FIX, &v);
    visor_evidence_scan(FIX, &v2);
    CHECK(visor_evidence_format_text(&v, txt_a, sizeof(txt_a)) > 0 &&
          visor_evidence_format_text(&v2, txt_b, sizeof(txt_b)) > 0 && strcmp(txt_a, txt_b) == 0, "text deterministic");
    CHECK(strstr(txt_a, "UNPARSED") && strstr(txt_a, "scope      SILICON (from silicon_observed=true)"), "text content");
    CHECK(visor_evidence_format_json(&v, js, sizeof(js)) > 0 && js[0] == '{' && strstr(js, "\"scope\":\"QEMU\""), "json");
    CHECK(visor_evidence_format_text(&v, txt_b, 32) == -1, "truncation fails closed");

    /* --- real evidence/ tree, read-only: format coverage and no upgrade --- */
    if (visor_evidence_scan("evidence", &v) == 0) {
        CHECK(v.scanned >= 70 && v.count == VISOR_EVIDENCE_MAX_ITEMS && v.truncated && v.matched >= 50,
              "real tree scanned (54 JSON of 76 files at ceb68d6)");
        CHECK(visor_evidence_load("evidence/PLAN_REUSE/d8c8246cf4f176a8327d31774cf3e31f39049606fe29af240f2d5a5dcd43de6f.json", &r) == 0 &&
              r.scope == VISOR_QUAL_HOST && r.name_hash_match == 1 && r.tree_state_known && !r.tree_dirty &&
              strcmp(r.commit, "7d562020abba34185ae180aa3db1ad5a7676c4b8") == 0 &&
              strstr(r.claim, "OMEGA_PLAN_REUSE_PASS=FAIL"), "real run receipt (host, content-addressed)");
        CHECK(visor_evidence_load("evidence/R12/af6c21f81303a0396ef66ffce5b7f37c85e3d780749095691bdadedef908ff75.json", &r) == 0 &&
              r.scope == VISOR_QUAL_SILICON && r.name_hash_match == 1, "real silicon receipt");
        CHECK(visor_evidence_load("evidence/R12/795e29fc23e5d3c8a3a5b742e859bd01c3aa07200219cd03d58817b599ff03fe.json", &r) == 0 &&
              r.scope == VISOR_QUAL_HOST, "real stand-in receipt stays HOST");
        CHECK(visor_evidence_load("evidence/omega_verify_qualification_receipt.json", &r) == 0 &&
              r.gates_run == 10 && r.gates_passed == 10 &&
              strcmp(r.commit, "8033c3805ce953d111c58c4da33a60678e15401d") == 0, "real milestone receipt");
        CHECK(visor_evidence_load("evidence/omega_blackwell_vector_qualification_receipt.json", &r) == 0 &&
              r.scope != VISOR_QUAL_SILICON, "gpu_architecture alone is not a silicon claim");
        CHECK(visor_evidence_load("evidence/omega_aarch64_qualification_receipt.json", &r) == 0 &&
              r.scope == VISOR_QUAL_QEMU && strcmp(r.scope_basis, "qemu_target") == 0, "real qemu_target receipt");
        CHECK(visor_evidence_load("evidence/omega_realization_synthesis_qualification_receipt.json", &r) == 0 &&
              r.scope == VISOR_QUAL_UNKNOWN, "QEMU gate name / modeled profile is not a QEMU claim");
    } else {
        fprintf(stderr, "note: evidence/ not found from cwd; real-tree checks skipped\n");
    }

    printf("%s %d/%d\n", g_pass == g_total ? "PASS" : "FAIL", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
