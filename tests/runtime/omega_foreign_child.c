#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
/* Deliberately small protocol: version, opcode, value. No Omega pointers,
 * capability references, secrets, World handles or inherited application FDs. */
typedef struct { uint32_t version, opcode; uint64_t value; } Request;
typedef struct { uint32_t version, status; uint64_t value; } Response;
static int read_full(int fd, void *p, size_t n) { size_t d=0; while(d<n){ssize_t r=read(fd,(char*)p+d,n-d);if(r<=0)return -1;d+=(size_t)r;}return 0; }
static int write_full(int fd, const void *p, size_t n) { size_t d=0; while(d<n){ssize_t r=write(fd,(const char*)p+d,n-d);if(r<=0)return -1;d+=(size_t)r;}return 0; }
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    int in=atoi(argv[1]), out=atoi(argv[2]); Request q;
    if (read_full(in,&q,sizeof(q)) != 0 || q.version != 1) return 3;
    if (q.opcode == 99) return 77; /* deterministic crash case */
    Response r = {1, 0, 0};
    if (q.opcode == 1) r.value = q.value * 3 + 1;
    else if (q.opcode == 2) { r.status = 1; r.value = q.value; } /* secret retrieval attempt */
    else r.status = 2;
    if (q.opcode == 98) { r.version = 999; } /* malformed reply case */
    return write_full(out,&r,sizeof(r)) == 0 ? 0 : 4;
}
