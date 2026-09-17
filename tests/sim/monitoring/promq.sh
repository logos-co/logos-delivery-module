#!/usr/bin/env bash
# promq.sh <promql>            — instant query (POST, no URL escaping worries)
# promq.sh --raw <path+query>  — raw API path
set -euo pipefail
if [ "${1:-}" = "--raw" ]; then
  exec docker run --rm --network logos-sim_sim curlimages/curl:8.11.1 -s --max-time 8 "http://10.0.0.11:9090$2"
fi
docker run --rm --network logos-sim_sim curlimages/curl:8.11.1 -s --max-time 8 \
  --data-urlencode "query=$1" http://10.0.0.11:9090/api/v1/query
