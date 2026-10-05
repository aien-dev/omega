/* allen.c -- thin host tool for ALLEN subject state (ARCH-0035 PROPOSED).
 *
 * Inspection and qualification only. Not a chat, not an agent runtime, not a
 * planner, not a daemon, not a Visor, not an operator shell. Every verb runs
 * to completion and exits; nothing here decides which faculty runs next.
 *
 *   allen make -o FILE --agent HEX32 --root HEX32 --prov HEX32
 *              [--intent REGIME,TARGET_NS]... [--cortex JOURNAL]
 *       Build a v0 subject object (test fixture: in production the object is
 *       committed into the AIENOS Store by continuity_subject.c; this verb
 *       writes the same bytes to a file for host qualification).
 *   allen inspect FILE          Decode, print identity, chain, bindings, intents.
 *   allen digest FILE           Print the subject id only.
 *   allen lineage JOURNAL       Print the Cortex lineage reference of a journal.
 *   allen seed-journal JOURNAL [TAG]
 *       Create a Cortex journal with one fixture record (the organism's genesis
 *       record stand-in; TAG makes two fixtures distinct lineages).
 *   allen publish FILE JOURNAL [--model HEX32]
 *       The host rig: capability root, World, the resident AIEN faculty
 *       (part A of R11: AIEN alone). Checks the memory binding (fail closed),
 *       records the identity binding, publishes every ACTIVE standing intent
 *       through rx_world_publish_external, waits for quiescence and prints
 *       what AIEN did with it. The model digest is printed as provenance and
 *       stored nowhere.
 *   allen probe JOURNAL
 *       The same rig started without ALLEN: how many goals does the World
 *       hold, and how many records does the journal hold? (negative control)
 *
 * Exit: 0 ok; 2 usage; 3 refused (decode, binding, publish); 4 rig failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allen/allen_bind.h"
#include "runtime/aienos_cap.h"
#include "runtime/rx_aien.h"
#include "runtime/rx_cortex.h"
#include "runtime/rx_cortex_record.h"
#include "runtime/rx_omega.h"
#include "runtime/rx_world.h"

#define SUBJ_EXTERNAL 7u
#define ISSUER 1u
#define RES_DEMAND 0x7711000ull
#define RES_SELECTION 0x7711001ull
#define JOURNAL_SUBJECTS 512u
#define SESSION 0x414c4c454e2d3030ull /* "ALLEN-00" */

static int usage(void)
{
    fprintf(stderr,
            "usage: allen make -o FILE --agent HEX --root HEX --prov HEX [--intent R,T]... [--cortex JOURNAL]\n"
            "       allen inspect FILE | digest FILE | lineage JOURNAL | seed-journal JOURNAL [TAG]\n"
            "       allen publish FILE JOURNAL [--model HEX] | probe JOURNAL\n");
    return 2;
}

/* ---- rig: the body ALLEN state is published into ---- */
typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxAienFaculty a;
    RxAienCaps acaps;
    RxObjRef demand, selection;
    RxCapRef ext_goal;
    int up;
} Rig;

static RxCapRef mint(Rig *r, uint32_t subject, uint64_t resource, uint32_t rights)
{
    AienosCapRef office, out = {UINT32_MAX, 0};
    aienos_cap_office(r->admin, &office);
    AienosCapMint m = {ISSUER, subject, resource, rights, 0, {UINT32_MAX, 0}, office};
    if (aienos_cap_mint(r->admin, &m, &out) != 0) out = (AienosCapRef){UINT32_MAX, 0};
    return (RxCapRef){out.cap_id, out.generation};
}

static int rig_start(Rig *r)
{
    const uint32_t RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    uint64_t z[RX_MAX_FIELDS] = {0};
    RxAienConfig cfg;
    RxAienInputs in;
    memset(r, 0, sizeof *r);
    if (aienos_cap_start(&r->admin, &r->view) != 0) return -1;
    if (rx_world_init_native(&r->w, r->view, 4, 1u << 20) != RX_OK) {
        aienos_cap_stop(r->admin, r->view);
        return -1;
    }
    r->up = 1;
    r->w.external_subject = SUBJ_EXTERNAL;
    if (rx_world_create(&r->w, RX_OT_DEMAND, RX_PERSIST_RESIDENT, RES_DEMAND, z, &r->demand) != RX_OK ||
        rx_world_create(&r->w, RX_OT_SELECTION, RX_PERSIST_RESIDENT, RES_SELECTION, z, &r->selection) != RX_OK)
        return -1;
    rx_aien_default_config(&cfg);
    in.demand = r->demand;
    in.selection = r->selection;
    if (rx_aien_create_objects(&r->a, &r->w, &cfg, &in) != RX_OK) return -1;
    for (uint32_t i = 0; i < RX_AIEN_RES_COUNT; i++)
        r->acaps.own[i] = mint(r, RX_AIEN_SUBJ, RX_AIEN_RES_BASE + i,
                               i == RX_AIEN_RES_PLACEMENT || i == RX_AIEN_RES_GOAL ? RX_RIGHT_READ : RW);
    r->acaps.demand = mint(r, RX_AIEN_SUBJ, RES_DEMAND, RX_RIGHT_READ);
    r->acaps.selection = mint(r, RX_AIEN_SUBJ, RES_SELECTION, RX_RIGHT_READ);
    /* The outside world may write the goal; ALLEN's intents arrive that way. */
    r->ext_goal = mint(r, SUBJ_EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_GOAL, RX_RIGHT_WRITE);
    return rx_aien_register(&r->a, &r->acaps) == RX_OK ? 0 : -1;
}

static void rig_stop(Rig *r)
{
    if (!r->up) return;
    rx_world_wait_quiescent(&r->w, 20000);
    rx_world_destroy(&r->w);
    aienos_cap_stop(r->admin, r->view);
    r->up = 0;
}

static uint64_t fld(Rig *r, RxObjRef o, uint32_t i)
{
    RxObject x;
    if (rx_world_read(&r->w, o, &x) != RX_OK) return UINT64_MAX;
    return x.field[i];
}

static uint32_t goals_in_world(Rig *r)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) {
        const RxObject *o = &r->w.objects[i];
        if (o->live && o->type == RX_OT_GOAL && o->field[0] != 0) n++;
    }
    return n;
}

/* ---- verbs ---- */
static void print_subject(const struct cs_subject *s, const uint8_t id[32])
{
    char h[65];
    allen_hex(id, 32, h);
    printf("ALLEN subject %s\n", h);
    allen_hex(s->agent, 32, h);
    printf("ALLEN agent %s\n", h);
    allen_hex(s->root, 32, h);
    printf("ALLEN root %s\n", h);
    allen_hex(s->previous, 32, h);
    printf("ALLEN chain sequence %llu previous %s\n", (unsigned long long)s->sequence, h);
    allen_hex(s->cortex, 32, h);
    printf("ALLEN cortex %s\n", h);
    allen_hex(s->provenance, 32, h);
    printf("ALLEN provenance %s origin %u\n", h, s->origin);
    printf("ALLEN intents %u knowledge %u\n", s->n_intents, s->n_knowledge);
    for (uint32_t i = 0; i < s->n_intents; i++) {
        const struct cs_intent *a = &s->in[i];
        allen_hex(a->id, 32, h);
        printf("ALLEN intent %s kind %u state %u since %llu regime %llu target_ns %llu\n", h, a->kind,
               a->state, (unsigned long long)a->since, (unsigned long long)a->payload[0],
               (unsigned long long)a->payload[1]);
    }
    for (uint32_t i = 0; i < s->n_knowledge; i++) {
        allen_hex(s->kn[i].digest, 32, h);
        printf("ALLEN knowledge %s record %llu since %llu\n", h, (unsigned long long)s->kn[i].record,
               (unsigned long long)s->kn[i].since);
    }
}

static int v_make(int argc, char **argv)
{
    static struct cs_subject s;
    static uint8_t buf[CC_MAX_OBJECT_BYTES];
    struct cr_view v;
    uint8_t prov[32] = {0}, id[32];
    const char *out = NULL, *journal = NULL, *why = NULL;
    size_t len = 0;
    int have_agent = 0, have_root = 0;
    memset(&v, 0, sizeof v);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--agent") && i + 1 < argc) have_agent = allen_unhex(argv[++i], v.root.agent_id, 32) == 0;
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) have_root = allen_unhex(argv[++i], v.root_id, 32) == 0;
        else if (!strcmp(argv[i], "--prov") && i + 1 < argc) { if (allen_unhex(argv[++i], prov, 32)) return usage(); }
        else if (!strcmp(argv[i], "--cortex") && i + 1 < argc) journal = argv[++i];
        else if (!strcmp(argv[i], "--intent") && i + 1 < argc) i++; /* second pass */
        else return usage();
    }
    if (!out || !have_agent || !have_root) return usage();
    cs_subject_genesis(&s, &v, prov, CS_ORIGIN_OPERATOR);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--intent") && i + 1 < argc) {
            uint64_t p[2];
            unsigned long long a, b;
            if (sscanf(argv[++i], "%llu,%llu", &a, &b) != 2) return usage();
            p[0] = a;
            p[1] = b;
            if (cs_subject_intend(&s, CS_INTENT_GOAL_LATENCY, p, NULL, &why) != CC_OK) {
                fprintf(stderr, "allen: intent refused: %s\n", why);
                return 3;
            }
        }
    }
    if (journal) {
        CxStore cx;
        uint8_t ref[32];
        if (cx_open(&cx, journal, JOURNAL_SUBJECTS, CX_OPEN_READONLY) != CX_OK) {
            fprintf(stderr, "allen: cannot open journal %s\n", journal);
            return 3;
        }
        if (allen_lineage(&cx, ref) != 0) {
            fprintf(stderr, "allen: journal has no genesis record; subject left unbound\n");
        } else {
            cs_subject_bind_cortex(&s, ref);
        }
        cx_close(&cx);
    }
    if (cs_subject_encode(&s, buf, sizeof buf, &len, &why) != CC_OK) {
        fprintf(stderr, "allen: encode refused: %s\n", why);
        return 3;
    }
    {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(buf, 1, len, f) != len) {
            fprintf(stderr, "allen: cannot write %s\n", out);
            return 4;
        }
        fclose(f);
    }
    cs_subject_id(buf, len, id);
    print_subject(&s, id);
    return 0;
}

static int v_inspect(const char *path, int digest_only)
{
    static struct cs_subject s;
    uint8_t id[32];
    const char *why = NULL;
    char h[65];
    if (allen_load(path, &s, id, &why) != 0) {
        fprintf(stderr, "allen: refused: %s\n", why ? why : "?");
        return 3;
    }
    if (digest_only) {
        allen_hex(id, 32, h);
        printf("%s\n", h);
        return 0;
    }
    print_subject(&s, id);
    return 0;
}

static int v_lineage(const char *journal)
{
    CxStore cx;
    uint8_t ref[32];
    char h[65];
    if (cx_open(&cx, journal, JOURNAL_SUBJECTS, CX_OPEN_READONLY) != CX_OK) {
        fprintf(stderr, "allen: cannot open journal %s\n", journal);
        return 3;
    }
    if (allen_lineage(&cx, ref) != 0) {
        printf("ALLEN lineage unbound (journal has no record)\n");
        cx_close(&cx);
        return 0;
    }
    allen_hex(ref, 32, h);
    printf("ALLEN lineage %s records %llu\n", h, (unsigned long long)cx.n);
    cx_close(&cx);
    return 0;
}

static int v_seed(const char *journal, uint64_t tag)
{
    CxStore cx;
    CxHeader hd;
    uint64_t payload[2] = {0x53454544ull, tag}; /* "SEED" + tag: fixture, not a claim */
    uint64_t id = 0;
    uint8_t ref[32];
    char h[65];
    if (cx_open(&cx, journal, JOURNAL_SUBJECTS, CX_OPEN_SYNC) != CX_OK) {
        fprintf(stderr, "allen: cannot create journal %s\n", journal);
        return 3;
    }
    if (cx.n != 0) {
        fprintf(stderr, "allen: journal already has %llu records\n", (unsigned long long)cx.n);
        cx_close(&cx);
        return 3;
    }
    memset(&hd, 0, sizeof hd);
    hd.cls = CX_OBSERVATION;
    hd.kind = CX_K_WORK_ACCEPTED;
    hd.subject = 1;
    hd.t = 1;
    hd.generation = 1;
    if (cx_append(&cx, &hd, payload, 2, &id) != CX_OK) {
        fprintf(stderr, "allen: cannot append the genesis record\n");
        cx_close(&cx);
        return 3;
    }
    allen_lineage(&cx, ref);
    allen_hex(ref, 32, h);
    printf("ALLEN seeded journal %s record %llu lineage %s\n", journal, (unsigned long long)id, h);
    cx_close(&cx);
    return 0;
}

static int v_publish(int argc, char **argv)
{
    static struct cs_subject s;
    static Rig r;
    CxStore cx;
    uint8_t id[32], model[32];
    int have_model = 0;
    const char *why = NULL;
    const char *path = argv[2], *journal = argv[3];
    char h[65];
    AllenBinding b;
    uint64_t goal_seq = 0, published = 0;
    int64_t last_cid = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) have_model = allen_unhex(argv[++i], model, 32) == 0;
        else return usage();
    }
    if (allen_load(path, &s, id, &why) != 0) {
        fprintf(stderr, "allen: refused: %s\n", why ? why : "?");
        return 3;
    }
    if (cx_open(&cx, journal, JOURNAL_SUBJECTS, CX_OPEN_SYNC) != CX_OK) {
        fprintf(stderr, "allen: cannot open journal %s\n", journal);
        return 3;
    }
    /* Memory binding first: a subject never acts on a journal that is not its own. */
    if (allen_check_memory(&s, &cx, &why) != 0) {
        fprintf(stderr, "allen: refused: %s\n", why);
        cx_close(&cx);
        return 3;
    }
    if (rig_start(&r) != 0) {
        fprintf(stderr, "allen: rig failed\n");
        rig_stop(&r);
        cx_close(&cx);
        return 4;
    }
    /* The organism's recorder: every crumb becomes a Cortex record (as the
     * living system does). ALLEN does not write Cortex itself. */
    if (rx_cortex_attach(&r.w, &cx, SESSION) != RX_OK) {
        fprintf(stderr, "allen: cortex attach failed\n");
        rig_stop(&r);
        cx_close(&cx);
        return 4;
    }
    allen_bind_identity(&r.w, &s, id, &b);
    print_subject(&s, id);
    allen_hex(b.agent, 32, h);
    printf("ALLEN binding external_subject %u agent %s sequence %llu\n", b.external_subject, h,
           (unsigned long long)b.sequence);
    allen_hex(s.cortex, 32, h);
    printf("ALLEN memory BOUND lineage %s journal %s\n", h, journal);
    if (have_model) {
        allen_hex(model, 32, h);
        printf("ALLEN model %s (provenance only; not in the subject object)\n", h);
    } else {
        printf("ALLEN model none (no model required)\n");
    }
    goal_seq = fld(&r, r.a.o.goal, 0);
    for (uint32_t i = 0; i < s.n_intents; i++) {
        RxMutation m[4];
        uint32_t n = allen_goal_mutations(&s.in[i], r.a.o.goal, ++goal_seq, m);
        if (n == 0) {
            goal_seq--;
            continue;
        }
        last_cid = rx_world_publish_external(&r.w, r.ext_goal, m, n);
        allen_hex(s.in[i].id, 32, h);
        if (last_cid <= 0) {
            printf("ALLEN publish intent %s REFUSED %lld\n", h, (long long)last_cid);
            rx_cortex_detach(&r.w);
            rig_stop(&r);
            cx_close(&cx);
            return 3;
        }
        printf("ALLEN publish intent %s -> EXTERNAL crumb %lld\n", h, (long long)last_cid);
        published++;
    }
    rx_world_wait_quiescent(&r.w, 20000);
    /* What AIEN did with it: readiness, not a call. */
    {
        uint64_t assess_acts = 0, in_episode = 0;
        for (uint64_t c = 1; c <= r.w.n_crumbs; c++) {
            const RxCrumb *k = rx_world_crumb(&r.w, c);
            if (!k || k->kind == RX_CRUMB_EXTERNAL || k->kind == RX_CRUMB_CREATE) continue;
            if (k->reaction == r.a.r_assess && k->faculty == RX_FACULTY_AIEN) {
                assess_acts++;
                if ((int64_t)k->episode == last_cid) in_episode++;
            }
        }
        printf("ALLEN goal object seq %llu regime %llu target_ns %llu intent_lo %016llx\n",
               (unsigned long long)fld(&r, r.a.o.goal, 0), (unsigned long long)fld(&r, r.a.o.goal, 1),
               (unsigned long long)fld(&r, r.a.o.goal, 2), (unsigned long long)fld(&r, r.a.o.goal, 3));
        printf("AIEN assess activations %llu in_episode_of_last_publish %llu assessment goal_seq %llu status %llu\n",
               (unsigned long long)assess_acts, (unsigned long long)in_episode,
               (unsigned long long)fld(&r, r.a.o.assessment, 0), (unsigned long long)fld(&r, r.a.o.assessment, 4));
        printf("ALLEN published %llu goals_in_world %u crumbs %llu journal_records %llu\n",
               (unsigned long long)published, goals_in_world(&r), (unsigned long long)r.w.n_crumbs,
               (unsigned long long)cx.n);
    }
    rx_cortex_detach(&r.w);
    rig_stop(&r);
    cx_close(&cx);
    return 0;
}

static int v_probe(const char *journal)
{
    static Rig r;
    CxStore cx;
    if (cx_open(&cx, journal, JOURNAL_SUBJECTS, CX_OPEN_READONLY) != CX_OK) {
        fprintf(stderr, "allen: cannot open journal %s\n", journal);
        return 3;
    }
    if (cx_verify_chain(&cx) != CX_OK) {
        fprintf(stderr, "allen: journal chain does not verify\n");
        cx_close(&cx);
        return 3;
    }
    if (rig_start(&r) != 0) {
        fprintf(stderr, "allen: rig failed\n");
        rig_stop(&r);
        cx_close(&cx);
        return 4;
    }
    rx_world_wait_quiescent(&r.w, 20000);
    printf("ALLEN probe journal_records %llu goals_in_world %u goal_seq %llu assessment_goal_seq %llu\n",
           (unsigned long long)cx.n, goals_in_world(&r), (unsigned long long)fld(&r, r.a.o.goal, 0),
           (unsigned long long)fld(&r, r.a.o.assessment, 0));
    rig_stop(&r);
    cx_close(&cx);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "make")) return v_make(argc, argv);
    if (!strcmp(argv[1], "inspect") && argc == 3) return v_inspect(argv[2], 0);
    if (!strcmp(argv[1], "digest") && argc == 3) return v_inspect(argv[2], 1);
    if (!strcmp(argv[1], "lineage") && argc == 3) return v_lineage(argv[2]);
    if (!strcmp(argv[1], "seed-journal") && (argc == 3 || argc == 4))
        return v_seed(argv[2], argc == 4 ? strtoull(argv[3], NULL, 10) : 0);
    if (!strcmp(argv[1], "publish") && argc >= 4) return v_publish(argc, argv);
    if (!strcmp(argv[1], "probe") && argc == 3) return v_probe(argv[2]);
    return usage();
}
