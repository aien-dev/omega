/* pd1_convert: turn a label-free TSV of retrospective machine runs into the
 * PD1REC1 fixed-width record stream with a SHA-256 chain.
 *
 * Input (stdin), one record per line, tab separated, integers only:
 *   src  q0  q1  q2  q3  q4  q5  r0  r1  cov
 *   src   u8   source campaign id (opaque small integer)
 *   q0..q3 u8  opaque knobs; 255 = not recorded
 *   q4    u32  opaque ordinal knob (position in its run series); 4294967295 = not recorded
 *   q5    i64  opaque time knob, micro units; -9223372036854775808 = not recorded
 *   r0    i64  outcome magnitude, micro units (latency); -1 = not recorded
 *   r1    u8   outcome class (0 ok, 1 fail, 255 unknown)
 *   cov   u32  covariate, milli units (host load); 4294967295 = not recorded
 * Output: PD1REC1 records to the file named in argv[1]; a human projection
 * (same opaque names) to stdout. Exit 0 and prints "records=N root=<hex>".
 *
 * PD1REC1 layout, little-endian, 112 bytes:
 *   0  magic[8]      "PD1REC1\0"
 *   8  version u16   1 (readers refuse other values)
 *  10  src u8
 *  11  r1 u8
 *  12  q0 u8  13 q1 u8  14 q2 u8  15 q3 u8
 *  16  seq u64       from 0
 *  24  q4 u32
 *  28  cov u32
 *  32  q5 i64
 *  40  r0 i64
 *  48  prev_hash[32] record_hash of seq-1, zero for seq 0
 *  80  record_hash[32] SHA-256 of bytes 0..79
 * No Python, no floats in any canonical byte. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* ---- compact SHA-256 (FIPS 180-4) ---- */
static const uint32_t K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void sha256_block(uint32_t h[8], const uint8_t p[64]) {
    uint32_t w[64], a,b,c,d,e,f,g,hh; int i;
    for (i=0;i<16;i++) w[i]=(uint32_t)p[4*i]<<24|(uint32_t)p[4*i+1]<<16|(uint32_t)p[4*i+2]<<8|p[4*i+3];
    for (;i<64;i++){uint32_t s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3),s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
    a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];hh=h[7];
    for (i=0;i<64;i++){uint32_t S1=ROR(e,6)^ROR(e,11)^ROR(e,25),ch=(e&f)^(~e&g),t1=hh+S1+ch+K[i]+w[i],S0=ROR(a,2)^ROR(a,13)^ROR(a,22),mj=(a&b)^(a&c)^(b&c),t2=S0+mj;hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}
static void sha256(const uint8_t *m, size_t n, uint8_t out[32]) {
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t blk[128]; size_t i; uint64_t bits=(uint64_t)n*8;
    for (i=0;i+64<=n;i+=64) sha256_block(h,m+i);
    size_t r=n-i; memcpy(blk,m+i,r); blk[r++]=0x80;
    size_t tot=(r<=56)?64:128; memset(blk+r,0,tot-r);
    for (int k=0;k<8;k++) blk[tot-1-k]=(uint8_t)(bits>>(8*k));
    sha256_block(h,blk); if (tot==128) sha256_block(h,blk+64);
    for (int k=0;k<8;k++){out[4*k]=h[k]>>24;out[4*k+1]=h[k]>>16;out[4*k+2]=h[k]>>8;out[4*k+3]=h[k];}
}
/* ---- LE writers ---- */
static void w16(uint8_t*p,uint16_t v){p[0]=v;p[1]=v>>8;}
static void w32(uint8_t*p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void w64(uint8_t*p,uint64_t v){for(int i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}

#define REC 112
/* --verify FILE: recompute the chain; refuse bad magic, version, hash or link. */
static int verify(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "verify: cannot open %s\n", path); return 3; }
    uint8_t rec[REC], prev[32] = {0}, h[32]; uint64_t seq = 0;
    while (fread(rec, 1, REC, f) == REC) {
        if (memcmp(rec, "PD1REC1\0", 8)) { fprintf(stderr, "verify: bad magic at %" PRIu64 "\n", seq); return 10; }
        if (rec[8] != 1 || rec[9] != 0) { fprintf(stderr, "verify: unknown version at %" PRIu64 "\n", seq); return 11; }
        uint64_t s = 0; for (int i=0;i<8;i++) s |= (uint64_t)rec[16+i] << (8*i);
        if (s != seq) { fprintf(stderr, "verify: seq gap at %" PRIu64 "\n", seq); return 12; }
        if (memcmp(rec+48, prev, 32)) { fprintf(stderr, "verify: chain broken at %" PRIu64 "\n", seq); return 13; }
        sha256(rec, 80, h);
        if (memcmp(h, rec+80, 32)) { fprintf(stderr, "verify: hash mismatch at %" PRIu64 "\n", seq); return 14; }
        memcpy(prev, rec+80, 32); seq++;
    }
    fclose(f);
    printf("verify: ok records=%" PRIu64 " root=", seq);
    for (int i=0;i<32;i++) printf("%02x", prev[i]);
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--verify")) return verify(argv[2]);
    if (argc != 2) { fprintf(stderr, "usage: pd1_convert OUTFILE < records.tsv | pd1_convert --verify FILE\n"); return 2; }
    FILE *out = fopen(argv[1], "wbx");           /* refuse to overwrite: evidence is append-only */
    if (!out) { fprintf(stderr, "pd1_convert: cannot create %s (exists?)\n", argv[1]); return 3; }
    uint8_t prev[32] = {0}, rec[REC]; char line[512]; uint64_t seq = 0;
    printf("seq\tsrc\tq0\tq1\tq2\tq3\tq4\tq5\tr0\tr1\tcov\n");
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned src,q0,q1,q2,q3,r1; unsigned long q4,cov; long long q5,r0;
        if (sscanf(line, "%u %u %u %u %u %lu %lld %lld %u %lu", &src,&q0,&q1,&q2,&q3,&q4,&q5,&r0,&r1,&cov) != 10) {
            fprintf(stderr, "pd1_convert: bad line %" PRIu64 ": %s", seq, line); fclose(out); return 4; }
        if (src>255||q0>255||q1>255||q2>255||q3>255||r1>255||q4>0xFFFFFFFFul||cov>0xFFFFFFFFul) {
            fprintf(stderr, "pd1_convert: range error at line %" PRIu64 "\n", seq); fclose(out); return 4; }
        memset(rec, 0, REC);
        memcpy(rec, "PD1REC1\0", 8); w16(rec+8, 1);
        rec[10]=(uint8_t)src; rec[11]=(uint8_t)r1; rec[12]=(uint8_t)q0; rec[13]=(uint8_t)q1; rec[14]=(uint8_t)q2; rec[15]=(uint8_t)q3;
        w64(rec+16, seq); w32(rec+24, (uint32_t)q4); w32(rec+28, (uint32_t)cov);
        w64(rec+32, (uint64_t)q5); w64(rec+40, (uint64_t)r0);
        memcpy(rec+48, prev, 32);
        sha256(rec, 80, rec+80); memcpy(prev, rec+80, 32);
        if (fwrite(rec, REC, 1, out) != 1) { fclose(out); return 5; }
        printf("%" PRIu64 "\t%u\t%u\t%u\t%u\t%u\t%lu\t%lld\t%lld\t%u\t%lu\n", seq, src,q0,q1,q2,q3,q4,q5,r0,r1,cov);
        seq++;
    }
    fclose(out);
    fprintf(stderr, "records=%" PRIu64 " root=", seq);
    for (int i=0;i<32;i++) fprintf(stderr, "%02x", prev[i]);
    fprintf(stderr, "\n");
    return 0;
}
