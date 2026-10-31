# Merge Mandiant unifiedlog_iterator outputs: names/messages from log-archive mode, timestamps from
# single-file mode (raw continuous ticks; Yuzu's timebase is 625/12 ns per tick). Pairs per (pid, thread).
import json,collections,os
K=625/12
full=collections.defaultdict(list)
for l in open('ioslog.jsonl'):
    e=json.loads(l); full[e['evidence'].split('yuzu.logarchive/',1)[1]].append(e)
out=collections.defaultdict(list)
for ev,rows in sorted(full.items()):
    sf='sf/'+ev.replace('/','_')+'.jsonl'
    A=collections.defaultdict(list); B=collections.defaultdict(list)
    for e in rows: A[(e['pid'],e['thread_id'])].append(e)
    for l in open(sf):
        e=json.loads(l); B[(e['pid'],e['thread_id'])].append(e)
    bad=0
    for k,a in A.items():
        b=B.get(k,[])
        if len(a)!=len(b): bad+=len(a); continue
        for x,y in zip(a,b):
            t=y['time']*K/1e9; p=x['process'].rsplit('/',1)[-1]
            out[x['boot_uuid'][:8]].append((t,'%8.2f %-18s %-5s %s %s| %s\n'%(t,p[:18],x['log_type'][:5],x['subsystem'],('['+x['category']+']') if x['category'] else '',x['message'].replace('\n',' ')[:600])))
    print(ev,len(rows),'unpaired',bad)
for b,r in out.items():
    r.sort(key=lambda z:z[0]); open('boot-%s.txt'%b,'w').writelines(z[1] for z in r)
    print(b,len(r),'%.0f-%.0f'%(r[0][0],r[-1][0]))
