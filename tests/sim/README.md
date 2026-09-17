# Service-discovery simulation in docker compose

One `logosdeliverynode` seed (in-process kademlia, fixed identity) plus N
logos-core daemons, each hosting `delivery_module` and its `libp2p_module`
dependency with the external discovery plugin. Every node runs in its own
container with its own address.

Addresses matter: a registrar (nim-libp2p `iptree.ipScore`) charges
`advertExpiry/32` (28 s) of waiting per address-prefix bit the advertiser
shares with the ads it already holds. Sequential container addresses in one
/24 share ~27 bits, so every remote registration waited 13-15 min; members
scattered over a /8 (`gen-members.py`) share ~13 bits and wait 0-5 min. Neither
side exposes the registrar parameters, so this spread is the only lever.

## Build

```bash
./build.sh
```

Runs the Linux nix builds inside `nixos/nix` (store kept in the docker volume
`logos-sim-nix`, so reruns are incremental) and assembles `logos-sim:local`:

| Artefact | Source |
|---|---|
| `logoscore` portable bundle | `logos-logoscore-cli` at the rev `tests/e2e` pins (`LOGOSCORE_CLI_REV`) |
| `delivery_module` | `install-portable` of this working tree (uncommitted changes included) |
| `libp2p_module` | `install-portable` at the rev `flake.lock` pins |
| `logosdeliverynode` | the `logos-delivery` rev `flake.nix` pins, with its nix closure |
| `openmetrics` | `install-portable` at the rev `flake.lock` pins |

## Run

```bash
./up.sh 20                            # seed + members m1..m20 (out/members.yml generated)
docker compose -f docker-compose.yml -f out/members.yml logs -f m1
./report.sh                           # discovery report over out/traces/*.trace
./down.sh
```

Knobs (environment): `SUBNET` (default `10.0.0.0/8`, also `up.sh <n> <subnet>`),
`LOOKUP_INTERVAL` (s, both sides), `START_JITTER` (s, member start spread),
`MAX_PURE` (seed's pure-libp2p peer budget), `SEED_LOG_LEVEL`,
`GRAFANA_PORT` (3000), `PROMETHEUS_PORT` (9090).

## Live dashboards

`up.sh` also starts Prometheus and Grafana on the sim network. Open
<http://localhost:3000> — no login, two provisioned dashboards:

| Dashboard | Shows |
|---|---|
| **Discovery overview** (home) | Fleet convergence: peers each node holds in its service tables, kademlia routing tables, registrar occupancy, advertiser backlog, registration and lookup rates, and a per-node table whose node column opens the detail dashboard. |
| **Node detail** | One node picked at the top left: its discovery state and advertising, its role as a registrar, the DHT, connections and dial latency, memory and event-loop load. |

Where the numbers come from:

- The **seed** serves the delivery library's own Prometheus registry on
  `:8008` (`--metrics-server`). It is one process, so its series also carry
  nim-libp2p's kademlia and service-discovery metrics.
- A **member** is three processes, so it serves `:9100` through the
  `openmetrics` module, which merges the delivery library's exposition text
  with libp2p_module's registry and labels each series with its module.
  `libp2p_module` holds `kad_*` and `cd_*`; `delivery_module` holds
  `logos_delivery_*` plus an idle second libp2p registry for its relay switch.

Because of that split, dashboard queries read gauges as
`max by (node) (...)` and counters as `sum by (node) (rate(...))`, which is
correct for both roles. Prometheus labels the seed's series `module=in-process`
so grouping by module works everywhere.

Panels are generated, not hand-edited:

```bash
monitoring/gen-dashboards.py            # rewrites the two dashboard JSON files
monitoring/check-dashboards.py m3       # runs every panel query against a live run
monitoring/promq.sh 'max by (node) (cd_service_table_peers)'
```

Grafana picks the files up within 10 s; edits made in the Grafana UI are not
written back, so change `gen-dashboards.py`.

Member configuration is `conf/member.json.tpl` rendered by `member.sh` with the
container address; `libp2p_module` is bound to the container address through
`LIBP2P_MODULE_CONFIG`. Traces (`LD_DISCO_TRACE`) land in `out/traces/`.
