/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded CPU calibration load. Does not access firmware or device controls. */
#define _GNU_SOURCE
#include <arm_neon.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int stop;
struct worker { int cpu, error; uint64_t cpu_ns, iterations; float result; };
static uint64_t cpu_time(void)
{
	struct timespec t;
	if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)) { perror("thread clock"); exit(1); }
	return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static void *run(void *arg)
{
	struct worker *w=arg;
	cpu_set_t set;
	uint64_t start, n=0;
	float32x4_t a[8], scale=vdupq_n_f32(.99999f), bias=vdupq_n_f32(.00001f);
	CPU_ZERO(&set); CPU_SET(w->cpu,&set);
	w->error=pthread_setaffinity_np(pthread_self(),sizeof(set),&set);
	if(w->error) return NULL;
	for(int i=0;i<8;i++) a[i]=vdupq_n_f32((float)i+2.0f);
	start=cpu_time();
	while(!atomic_load_explicit(&stop,memory_order_relaxed)) {
		for(int i=0;i<4096;i++) {
			for(int j=0;j<8;j++) a[j]=vfmaq_f32(bias,a[j],scale);
			/* Keep all independent vectors live, including when rounded to a fixed point. */
			__asm__ volatile("" : "+w"(a[0]), "+w"(a[1]), "+w"(a[2]), "+w"(a[3]),
				"+w"(a[4]), "+w"(a[5]), "+w"(a[6]), "+w"(a[7]));
		}
		n+=4096;
	}
	w->cpu_ns=cpu_time()-start; w->iterations=n; w->result=0;
	for(int i=0;i<8;i++) w->result+=vaddvq_f32(a[i]);
	return NULL;
}
int main(int argc,char **argv)
{
	pthread_t threads[10]; struct worker w[10]={0};
	char *end; unsigned long seconds;
	int p,failed=0;
	if(argc!=3 || (strcmp(argv[1],"p") && strcmp(argv[1],"e"))) return 2;
	seconds=strtoul(argv[2],&end,10); if(*end || seconds<1 || seconds>120) return 2;
	p=!strcmp(argv[1],"p");
	for(int i=0;i<10;i++) {
		char path[160]; unsigned long long midr; FILE *f;
		w[i].cpu=i<5?i+(p?5:0):i+5+(p?5:0);
		snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%d/regs/identification/midr_el1",w[i].cpu);
		f=fopen(path,"r"); if(!f || fscanf(f,"%llx",&midr)!=1) return 1; fclose(f);
		if(((midr>>4)&0xfff)!=(unsigned int)(p?0xd85:0xd87)) return 1;
		if(pthread_create(&threads[i],NULL,run,&w[i])) exit(1);
	}
	sleep(seconds); atomic_store(&stop,1);
	for(int i=0;i<10;i++) {
		pthread_join(threads[i],NULL);
		printf("{\"cpu\":%d,\"cpu_ns\":%llu,\"iterations\":%llu,\"result\":%.6f,\"affinity_error\":%d}\n",w[i].cpu,(unsigned long long)w[i].cpu_ns,(unsigned long long)w[i].iterations,w[i].result,w[i].error);
		if(w[i].error || w[i].cpu_ns<seconds*UINT64_C(700000000)) failed=1;
	}
	return failed;
}
