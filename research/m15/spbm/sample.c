/* SPDX-License-Identifier: GPL-2.0-only */
/* Calibration observations only: no MMIO access and no hardware control. */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(1); }
	return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static uint64_t read_value(const char *dir, const char *kind, int n, const char *suffix)
{
	char path[512], buf[80], *end;
	int fd;
	ssize_t got;
	unsigned long long value;
	if (snprintf(path, sizeof(path), "%s/%s%d_%s", dir, kind, n, suffix) >= (int)sizeof(path)) exit(1);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) { perror(path); exit(1); }
	got = read(fd, buf, sizeof(buf)-1);
	if (got <= 0) { perror(path); close(fd); exit(1); }
	close(fd);
	buf[got] = 0; errno = 0;
	value = strtoull(buf, &end, 10);
	if (errno || end == buf || (*end != '\n' && *end != 0) || buf[0] == '-') exit(1);
	return value;
}
static void check_label(const char *dir, const char *kind, int n, const char *expect)
{
	char path[512], buf[80];
	FILE *f;
	if (snprintf(path,sizeof(path),"%s/%s%d_label",dir,kind,n)>=(int)sizeof(path)) exit(1);
	f=fopen(path,"r");
	if (!f || !fgets(buf,sizeof(buf),f)) { perror(path); exit(1); }
	fclose(f); buf[strcspn(buf,"\n")]=0;
	if (strcmp(buf,expect)) { fprintf(stderr,"wrong sensor label: %s\n",path); exit(1); }
}

int main(int argc, char **argv)
{
	const char *el[] = {"pkg","cpu_e","cpu_p","gpc_unverified","gpm"};
	const char *pl[] = {"sys_total","soc_pkg","cpu_e","cpu_p","gpu"};
	uint64_t prev[5] = {0}, nv_prev = 0, first, deadline;
	unsigned long seconds;
	char *end;
	void *lib, *gpu = NULL;
	int (*init)(void), (*shutdown_nv)(void), (*handle)(unsigned int, void **);
	int (*energy_nv)(void *, unsigned long long *), (*power_nv)(void *, unsigned int *);
	FILE *out;
	int fd;
	if (argc != 4) { fprintf(stderr,"usage: sample hwmon-dir seconds NEW-output.jsonl\n"); return 2; }
	errno=0; seconds=strtoul(argv[2],&end,10);
	if (errno || *end || seconds<1 || seconds>120) return 2;
	for (int i=0;i<5;i++) {
		check_label(argv[1],"energy",i+1,el[i]);
		check_label(argv[1],"power",i+1,pl[i]);
	}
	lib=dlopen("libnvidia-ml.so.1",RTLD_NOW|RTLD_LOCAL);
	if (!lib) { fprintf(stderr,"NVML: %s\n",dlerror()); return 1; }
#define SYM(dst,name) do { *(void **)(&(dst))=dlsym(lib,name); if (!(dst)) return 1; } while (0)
	SYM(init,"nvmlInit_v2"); SYM(shutdown_nv,"nvmlShutdown");
	SYM(handle,"nvmlDeviceGetHandleByIndex_v2");
	SYM(energy_nv,"nvmlDeviceGetTotalEnergyConsumption"); SYM(power_nv,"nvmlDeviceGetPowerUsage");
	if (init() || handle(0,&gpu)) return 1;
	fd=open(argv[3],O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
	if (fd<0) { perror(argv[3]); return 1; }
	out=fdopen(fd,"w"); if (!out) return 1;
	first=now_ns(); deadline=first;
	for (unsigned int seq=0;seq<=seconds*10;seq++) {
		uint64_t begin,end_ns,nv_begin,nv_end,e[5],p[5],ov[5];
		unsigned long long nv_mj;
		unsigned int nv_mw;
		struct timespec when={(time_t)(deadline/1000000000),(long)(deadline%1000000000)};
		int rc;
		do { rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&when,NULL); } while(rc==EINTR);
		if (rc) return 1;
		begin=now_ns();
		if (begin-deadline>100000000) { fprintf(stderr,"sample missed deadline by >100 ms\n"); return 1; }
		for (int i=0;i<5;i++) {
			e[i]=read_value(argv[1],"energy",i+1,"input");
			ov[i]=read_value(argv[1],"energy",i+1,"overflow_raw");
			p[i]=read_value(argv[1],"power",i+1,"input");
			if (ov[i] || (seq && e[i]<prev[i]) || e[i]>UINT32_MAX*UINT64_C(1000) || e[i]%1000 || p[i]>600000000) {
				fprintf(stderr,"invalid/overflowed sensor %d\n",i+1); return 1;
			}
			/* Refuse a window that could wrap even at a 600 W upper bound. */
			if (!seq && UINT32_MAX*UINT64_C(1000)-e[i] < seconds*UINT64_C(600000000)) return 1;
			prev[i]=e[i];
		}
		end_ns=now_ns();
		if (end_ns-begin>10000000) { fprintf(stderr,"sensor read span >10 ms\n"); return 1; }
		nv_begin=now_ns();
		if (energy_nv(gpu,&nv_mj) || power_nv(gpu,&nv_mw) || (seq && nv_mj<nv_prev)) return 1;
		nv_prev=nv_mj; nv_end=now_ns();
		fprintf(out,"{\"sample\":%u,\"read_begin_ns\":%"PRIu64",\"read_end_ns\":%"PRIu64",\"energy_uj\":[",seq,begin,end_ns);
		for(int i=0;i<5;i++) fprintf(out,"%s%"PRIu64,i?",":"",e[i]);
		fputs("],\"power_uw\":[",out);
		for(int i=0;i<5;i++) fprintf(out,"%s%"PRIu64,i?",":"",p[i]);
		fprintf(out,"],\"overflow_raw\":[0,0,0,0,0],\"nvml_begin_ns\":%"PRIu64",\"nvml_end_ns\":%"PRIu64",\"nvml_energy_mj\":%llu,\"nvml_power_mw\":%u}\n",nv_begin,nv_end,nv_mj,nv_mw);
		if (fflush(out) || ferror(out)) return 1;
		deadline+=100000000;
	}
	if (fsync(fd) || fclose(out)) return 1;
	if (shutdown_nv()) return 1;
	dlclose(lib); return 0;
}
