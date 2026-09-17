#!/usr/bin/env python3
"""Runs every panel query of the provisioned dashboards against a running
sim's prometheus and reports the ones that error or return nothing.

    monitoring/check-dashboards.py [node]     # default node: m1

`$node` in the node dashboard is substituted with the given node. An EMPTY
line is not always a defect (a counter no node has incremented yet reads
empty), but an ERROR line always is.
"""
import json, subprocess, sys, glob, os

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def q(expr):
    out = subprocess.run(
        ["docker", "run", "--rm", "--network", "logos-sim_sim",
         "curlimages/curl:8.11.1", "-s", "--max-time", "8",
         "--data-urlencode", f"query={expr}",
         "http://10.0.0.11:9090/api/v1/query"],
        capture_output=True, text=True).stdout
    try:
        d = json.loads(out)
    except Exception:
        return None, "no response"
    if d.get("status") != "success":
        return None, d.get("error", "")[:100]
    return d["data"]["result"], None

node = sys.argv[1] if len(sys.argv) > 1 else "m1"
bad = 0
for path in sorted(glob.glob(os.path.join(here, "monitoring", "grafana", "dashboards", "*.json"))):
    dash = json.load(open(path))
    print(f"=== {os.path.basename(path)}")
    for p in dash["panels"]:
        if p["type"] == "row":
            continue
        for t in p.get("targets", []):
            expr = t["expr"].replace("$node", node)
            res, err = q(expr)
            if err:
                print(f"  ERROR  {p['title']!r} [{t['refId']}]: {err}")
                bad += 1
            elif not res:
                print(f"  EMPTY  {p['title']!r} [{t['refId']}]: {expr[:90]}")
                bad += 1
print("panels with empty/erroring queries:", bad)
