/* SPDX-License-Identifier: GPL-2.0-only */
/* Deterministic reduction of the calibration sampler's exact JSONL format.
 * This is not the R15 performance reducer and cannot issue an R15 PASS.
 */
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sample { unsigned seq; uint64_t begin,end,e[5],p[5],nv_begin,nv_end,nv_e,nv_p; };
static void fail(const char *s) { fprintf(stderr,"calibration: %s\n",s); exit(1); }
static int parse(const char *s,struct sample *v)
{
	int n=0,got=sscanf(s,"{\"sample\":%u,\"read_begin_ns\":%"SCNu64",\"read_end_ns\":%"SCNu64",\"energy_uj\":[%"SCNu64",%"SCNu64",%"SCNu64",%"SCNu64",%"SCNu64"],\"power_uw\":[%"SCNu64",%"SCNu64",%"SCNu64",%"SCNu64",%"SCNu64"],\"overflow_raw\":[0,0,0,0,0],\"nvml_begin_ns\":%"SCNu64",\"nvml_end_ns\":%"SCNu64",\"nvml_energy_mj\":%"SCNu64",\"nvml_power_mw\":%"SCNu64"}%n",
		&v->seq,&v->begin,&v->end,&v->e[0],&v->e[1],&v->e[2],&v->e[3],&v->e[4],
		&v->p[0],&v->p[1],&v->p[2],&v->p[3],&v->p[4],&v->nv_begin,&v->nv_end,&v->nv_e,&v->nv_p,&n);
	return got==17 && n>0 && (s[n]=='\n' || s[n]==0);
}
static double mid(const struct sample *v) { return v->begin+(v->end-v->begin)/2.0; }
static double nv_mid(const struct sample *v) { return v->nv_begin+(v->nv_end-v->nv_begin)/2.0; }
static void reduce(const char *file,const char *phase,int round,int comma)
{
	FILE *f=fopen(file,"r"); char line[2048];
	struct sample a={0},prev={0},cur={0};
	double integral[5]={0},nv_integral=0,max_span=0,max_gap=0;
	unsigned count=0;
	if(!f) fail(file);
	while(fgets(line,sizeof(line),f)) {
		if(!parse(line,&cur) || cur.seq!=count || cur.end<cur.begin || cur.end-cur.begin>10000000 || cur.nv_begin<cur.end || cur.nv_end<cur.nv_begin) fail("bad sample record");
		if(cur.end-cur.begin>max_span) max_span=cur.end-cur.begin;
		for(int i=0;i<5;i++) if(cur.e[i]>UINT32_MAX*UINT64_C(1000) || cur.e[i]%1000 || cur.p[i]>600000000) fail("invalid sensor value");
		if(!count) a=cur;
		else {
			double dt=(mid(&cur)-mid(&prev))/1e9;
			if(cur.begin<=prev.end || dt>.3) fail("nonmonotonic or missing sample");
			if(dt>max_gap) max_gap=dt;
			for(int i=0;i<5;i++) {
				if(cur.e[i]<prev.e[i]) fail("energy counter decreased");
				integral[i]+=dt*(cur.p[i]+prev.p[i])/2e6;
			}
			if(cur.nv_e<prev.nv_e || cur.nv_begin<=prev.nv_end) fail("NVML counter/time decreased");
			nv_integral+=(nv_mid(&cur)-nv_mid(&prev))/1e9*(cur.nv_p+prev.nv_p)/2e3;
		}
		prev=cur;count++;
	}
	if(ferror(f) || count!=101) fail("missing samples");
	fclose(f);
	double dt=(mid(&cur)-mid(&a))/1e9,ndt=(nv_mid(&cur)-nv_mid(&a))/1e9,ew[5];
	for(int i=0;i<5;i++) ew[i]=(cur.e[i]-a.e[i])/dt/1e6;
	printf("%s{\"phase\":\"%s\",\"round\":%d,\"samples\":%u,\"seconds\":%.6f,\"energy_mean_w\":[",comma?",\n":"",phase,round,count,dt);
	for(int i=0;i<5;i++) printf("%s%.6f",i?",":"",ew[i]);
	fputs("],\"integrated_power_mean_w\":[",stdout);
	for(int i=0;i<5;i++) printf("%s%.6f",i?",":"",integral[i]/dt);
	printf("],\"nvml_energy_mean_w\":%.6f,\"nvml_power_mean_w\":%.6f,\"gpc_unverified\":true,\"max_read_span_us\":%.3f,\"max_sample_gap_ms\":%.3f}",
		(cur.nv_e-a.nv_e)/ndt/1e3,nv_integral/ndt,max_span/1e3,max_gap*1e3);
}
int main(int argc,char **argv)
{
	const char *phase[]={"idle","p","e"};char path[1024];int n=0;
	if(argc!=2) { fprintf(stderr,"usage: reduce run-directory\n"); return 2; }
	fputs("{\"schema\":\"AIEN_SPBM_CALIBRATION_OBSERVATIONS_V2\",\"qualified\":false,\"R15_PASS\":false,\"energy_order\":[\"pkg\",\"cpu_e\",\"cpu_p\",\"gpc_unverified\",\"gpm\"],\"power_order\":[\"sys_total\",\"soc_pkg\",\"cpu_e\",\"cpu_p\",\"gpu\"],\"windows\":[\n",stdout);
	for(int round=1;round<=3;round++) for(int i=0;i<3;i++) {
		if(snprintf(path,sizeof(path),"%s/%s-%d.jsonl",argv[1],phase[i],round)>=(int)sizeof(path)) fail("path too long");
		reduce(path,phase[i],round,n++);
	}
	fputs("\n]}\n",stdout);
	return ferror(stdout)?1:0;
}
