import json,sys,glob,os
R=os.path.join(os.path.dirname(os.path.abspath(__file__)),'runs')
def load(f):
    try: return [json.loads(l) for l in open(f) if l.strip().startswith('{')]
    except FileNotFoundError: return []
def summ(rows):
    out={}
    for r in rows:
        if r.get('mode')!='sustain': continue
        out.setdefault((r['cpu'],r['kind']),[]).append(r)
    for k,v in sorted(out.items()):
        v.sort(key=lambda r:r['second'])
        g=[r['eff_ghz'] for r in v]; w=[r.get('pkg_w') or 0 for r in v]
        drop=next((r['second'] for r in v if r['eff_ghz']<3.7),None) if k[0]>=5 else None
        early=sum(g[:10])/max(1,len(g[:10])); late=g[15:] and sum(g[15:])/len(g[15:])
        print(f"   cpu{k[0]:>2} {k[1]:5} secs={len(v):3} GHz first10={early:.3f} after15={late if late else float('nan'):.3f} min={min(g):.3f}  drop_at_s={drop}  W first10={sum(w[:10])/max(1,len(w[:10])):.1f} after15={(sum(w[15:])/len(w[15:])) if w[15:] else float('nan'):.1f}  maxT={max(r['max_temp_mc'] for r in v)/1000:.1f}C")
for tag,d in [('AFTERNOON',f'{R}/hwchar-20260928'),('POST-POWER-OFF',f'{R}/hwchar-postboot-unloaded')]:
    for secs in (30,120):
        print(f"== {tag} sustain-{secs}"); summ(load(f'{d}/sustain-{secs}.jsonl'))
for tag,d in [('AFTERNOON burst',f'{R}/hwchar-burst-20260928'),('POST burst unloaded',f'{R}/hwchar-burst-postboot-unloaded'),('POST burst loaded',f'{R}/hwchar-burst-postboot-loaded')]:
    print(f"== {tag}")
    for f in sorted(glob.glob(f'{d}/*.jsonl')):
        print('  ',os.path.basename(f)); summ(load(f))
