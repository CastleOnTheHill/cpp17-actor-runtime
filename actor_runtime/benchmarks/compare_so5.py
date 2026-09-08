"""Matched, fixed-budget native/SObjectizer acceptance gate; never drop slow cases."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import statistics
import time
from run_suite import execute, matrix


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--so5', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--revision', required=True)
    p.add_argument('--repeats', type=int, default=5)
    p.add_argument('--total-rounds', type=int, default=4000000)
    p.add_argument('--minimum-ratio', type=float, default=.8)
    p.add_argument('--quick', action='store_true')
    a = p.parse_args()
    if a.repeats<1 or a.total_rounds<64 or not 0<a.minimum_ratio<=1:
        p.error('invalid repetition/count/ratio')
    a.output.mkdir(parents=True, exist_ok=False)
    cases = [c for c in matrix() if c['binary']=='native_echo']
    if a.quick:
        cases = [c for c in cases if (c['workers'],c['clients'],c['window'],c['payload'],c['hotspot'])
                 in ((1,1,1,64,False),(4,8,1,64,False))]
    for c in cases:
        del c['binary']
    binaries = {'native':a.native.resolve(), 'so5':a.so5.resolve()}
    manifest = {'revision':a.revision, 'utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                'repeats':a.repeats, 'total_rounds':a.total_rounds, 'minimum_ratio':a.minimum_ratio,
                'cpus':'0-7', 'sample':16, 'warmup':1000, 'scope':'matched event echo; batch1',
                'binaries':{k:{'path':str(v),'sha256':hashlib.sha256(v.read_bytes()).hexdigest()} for k,v in binaries.items()}}
    (a.output/'environment.json').write_text(json.dumps(manifest,indent=2)+'\n')
    rows=[]
    for repetition in range(a.repeats):
        order=[(case,name) for case in cases for name in binaries]
        random.Random(20260909+repetition).shuffle(order)
        for case,name in order:
            rounds=max(16,a.total_rounds//case['clients'])
            cmd=['taskset','-c','0-7',str(binaries[name])]
            for key in ('workers','clients','window','payload','batch'):
                cmd += ['--'+key,str(case[key])]
            if case['hotspot']: cmd += ['--hotspot']
            cmd += ['--rounds',str(rounds),'--warmup','1000','--sample','16','--timeout','110']
            r=execute(cmd,120); r.update(case=case,framework=name,repetition=repetition)
            try:
                m=json.loads(r['stdout'].strip().splitlines()[-1])
                assert r['returncode']==0 and m['round_trips']==rounds*case['clients']
                assert m['rtt_samples']==((rounds+15)//16)*case['clients']
                assert m['seconds']>0 and m['rps']>0
                r['measurement']=m
            except (ValueError,IndexError,AssertionError): r['validation_error']=True
            with (a.output/'raw.jsonl').open('a') as f: f.write(json.dumps(r)+'\n')
            if 'validation_error' in r: raise RuntimeError(r)
            rows.append(r)
            print(f"run {repetition+1}/{a.repeats} {name} {case}: {m['rps']:.0f} rps {m['seconds']:.3f}s",flush=True)
    summary=[]
    for case in cases:
        result={'case':case}
        for name in binaries:
            ms=[r['measurement'] for r in rows if r['case']==case and r['framework']==name]
            result[name]={'rps':statistics.median(m['rps'] for m in ms),
                          'min_rps':min(m['rps'] for m in ms),'max_rps':max(m['rps'] for m in ms),
                          'p99_us':statistics.median(m['rtt_p99_us'] for m in ms),
                          'min_seconds':min(m['seconds'] for m in ms)}
        result['ratio']=result['native']['rps']/result['so5']['rps']
        result['pass']=result['ratio']>=a.minimum_ratio
        summary.append(result)
    (a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    failed=[r for r in summary if not r['pass']]
    print(json.dumps(summary,indent=2),flush=True)
    if failed: raise SystemExit(f'FAIL: {len(failed)}/{len(summary)} cases below {a.minimum_ratio:.0%} of SO5')
    print(f'PASS all {len(summary)} matched cases >= {a.minimum_ratio:.0%} of SO5',flush=True)


if __name__=='__main__': main()
