#!/usr/bin/env python3
"""Service-discovery report over the member trace files of a mixed cluster run.

Per member (nN.trace): backend bring-up, advertise outcome, every lookup with
its record count and the peer-id suffixes it returned; aggregates: success
rate, discovery latency, coverage of the expected id set, errors.
"""
import glob, os, re, statistics, sys
from datetime import datetime, timedelta

out = sys.argv[1]
expected = int(sys.argv[2])  # members + seed
traces = sorted(glob.glob(os.path.join(out, "*.trace")))

ts_re = re.compile(r"^\[(\d\d:\d\d:\d\d)\] (.*)$")
lookup_re = re.compile(r"discoLookup\s+OK\s+key=(\S+) records=(\d+) peers=\[([^\]]*)\]")

def parse_ts(s, day):
    return datetime.combine(day, datetime.strptime(s, "%H:%M:%S").time())

rows = []
all_ids = set()
for path in traces:
    n = os.path.basename(path)[: -len(".trace")]
    day = datetime.fromtimestamp(os.path.getmtime(path)).date()
    lines = open(path).read().splitlines()
    ready = advert = first_hit = None
    lookups = []
    errors = 0
    seen = set()
    prev = None
    for ln in lines:
        m = ts_re.match(ln)
        if not m:
            continue
        t, msg = parse_ts(m.group(1), day), m.group(2)
        if prev and t < prev - timedelta(hours=1):
            t += timedelta(days=1)  # midnight wrap
        prev = t
        if "backend ready" in msg and ready is None:
            ready = t
        elif msg.startswith("discoStartAdvertising") and "OK" in msg and advert is None:
            advert = t
        elif "TRANSPORT-ERR" in msg or "REFUSED" in msg or "UNAVAILABLE" in msg:
            errors += 1
        lm = lookup_re.search(msg)
        if lm and lm.group(1) == "/logos/delivery":
            cnt = int(lm.group(2))
            ids = [p for p in lm.group(3).split(",") if p]
            seen.update(ids)
            all_ids.update(ids)
            lookups.append((t, cnt))
            if cnt > 0 and first_hit is None:
                first_hit = t
    rows.append(dict(n=n, ready=ready, advert=advert, first_hit=first_hit, lookups=lookups, errors=errors, seen=seen))

members = len(rows)
ok_backend = sum(1 for r in rows if r["ready"])
ok_advert = sum(1 for r in rows if r["advert"])
with_hit = [r for r in rows if r["first_hit"]]
latencies = [(r["first_hit"] - r["ready"]).total_seconds() for r in with_hit if r["ready"]]
per_lookup = [c for r in rows for _, c in r["lookups"]]
nonempty = [c for c in per_lookup if c > 0]
coverage = [len(r["seen"]) for r in rows]
last_counts = [r["lookups"][-1][1] for r in rows if r["lookups"]]
total_lookups = len(per_lookup)
empty = total_lookups - len(nonempty)

def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p * (len(xs) - 1))))] if xs else float("nan")

print(f"members: {members}  expected ids in the DHT: {expected}  distinct ids seen anywhere: {len(all_ids)}")
print(f"backend ready: {ok_backend}/{members}   advertised /logos/delivery: {ok_advert}/{members}   plugin errors: {sum(r['errors'] for r in rows)}")
print(f"members whose lookups found peers: {len(with_hit)}/{members}")
if latencies:
    print(f"discovery latency (backend ready -> first non-empty lookup): median {statistics.median(latencies):.0f}s  p90 {pct(latencies,0.9):.0f}s  max {max(latencies):.0f}s")
print(f"lookups: {total_lookups} total, {empty} empty ({(empty/total_lookups*100 if total_lookups else 0):.1f}%)")
if nonempty:
    print(f"records per non-empty lookup: median {statistics.median(nonempty):.0f}  p10 {pct(nonempty,0.1)}  max {max(nonempty)}   (expected set {expected})")
if last_counts:
    print(f"records in each member's LAST lookup: median {statistics.median(last_counts):.0f}  min {min(last_counts)}  max {max(last_counts)}")
if coverage:
    print(f"coverage per member (distinct ids seen over all lookups): median {statistics.median(coverage):.0f}  min {min(coverage)}  max {max(coverage)}  of {expected}")
    full = sum(1 for c in coverage if c >= expected)
    print(f"members that saw the whole set at least once: {full}/{members};  >= 80%: {sum(1 for c in coverage if c >= 0.8*expected)}/{members}")
print()
print("per member: name  ready  advert  first-hit(s after ready)  lookups  last  coverage  errors")
for r in rows:
    lat = f"{(r['first_hit']-r['ready']).total_seconds():4.0f}" if r["first_hit"] and r["ready"] else "  --"
    print(f"  {r['n']:<14} {'y' if r['ready'] else 'N'}      {'y' if r['advert'] else 'N'}       {lat}   {len(r['lookups']):3d}   {r['lookups'][-1][1] if r['lookups'] else 0:3d}    {len(r['seen']):3d}      {r['errors']}")
