#!/usr/bin/env python3
"""Summarize RAVEN_TRACE_HANDLE_PHASES for the single parent/child handle-latency fixture.

Run one fixture at a time with stderr redirected to a file, then pass that file here.
Request numbers identify the one measured channel; do not combine runs or general API traces.
"""
import sys, collections
rows=[]
for line in open(sys.argv[1]):
    offset = line.find('handle-phase ')
    if offset >= 0:
        line = line[offset:]
        _,ns,pid,fd,seq,phase,kind=line.split()
        rows.append((int(ns),pid,fd,seq,phase,kind))
starts={}; dispatch={}; sent={}; samples=collections.defaultdict(lambda:collections.defaultdict(list)); timeouts=0
for ns,pid,fd,seq,phase,kind in sorted(rows):
    if phase=='send': starts[seq]=(ns,kind)
    elif phase=='dispatch': dispatch[seq]=ns
    elif phase=='answer-send': sent[seq]=ns
    elif phase=='timeout': timeouts+=1
    elif phase=='answer-receive' and seq in starts and seq in dispatch and seq in sent:
        start,k=starts.pop(seq); d=dispatch.pop(seq); a=sent.pop(seq)
        for name,val in [('roundtrip',ns-start),('dispatch',d-start),('service',a-d),('reply',ns-a)]: samples[k][name].append(val/1e6)
for kind,metrics in sorted(samples.items()):
    for name,values in metrics.items():
        values.sort(); print(kind,name,'count',len(values),'p50_ms',round(values[len(values)//2],4),'p95_ms',round(values[int((len(values)-1)*.95)],4))
print('timeouts',timeouts,'events',len(rows))
