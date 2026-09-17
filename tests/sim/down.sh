#!/usr/bin/env bash
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
docker compose -f "$here/docker-compose.yml" -f "$here/out/members.yml" down "$@"
