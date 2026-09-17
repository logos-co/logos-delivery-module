#!/usr/bin/env python3
"""Generates the two provisioned Grafana dashboards into ./grafana/dashboards.

Panels are described here rather than hand-written as JSON: the sim's metric
shapes are regular enough that a few helpers cover every panel, and the two
dashboards stay in sync when a query changes.

Metric shapes worth knowing (see README):
  * the seed is one process: its series carry no `module` label;
  * a member is scraped through its openmetrics module, which labels every
    series with the module it came from — `libp2p_module` holds the kademlia
    and service-discovery registries (`kad_*`, `cd_*`), `delivery_module`
    holds the delivery library's own registry (`logos_delivery_*`) plus a
    second, idle libp2p registry for its relay switch.
So `max by (node) (...)` reads a gauge correctly for both roles (the idle copy
is 0), and `sum by (node) (rate(...))` does the same for counters.
"""
import json, os

DS = {"type": "prometheus", "uid": "sim-prom"}
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "grafana", "dashboards")

# ---------------------------------------------------------------- helpers --

class Layout:
    """Places panels left to right, wrapping at 24 columns."""
    def __init__(self):
        self.panels, self.x, self.y, self.row_h, self._id = [], 0, 0, 0, 0

    def add(self, panel, w, h):
        if self.x + w > 24:
            self.x, self.y = 0, self.y + self.row_h
            self.row_h = 0
        self._id += 1
        panel["id"] = self._id
        panel["gridPos"] = {"h": h, "w": w, "x": self.x, "y": self.y}
        self.x += w
        self.row_h = max(self.row_h, h)
        self.panels.append(panel)
        return panel

    def row(self, title):
        if self.x:
            self.x, self.y = 0, self.y + self.row_h
            self.row_h = 0
        self._id += 1
        self.panels.append({
            "id": self._id, "type": "row", "title": title, "collapsed": False,
            "gridPos": {"h": 1, "w": 24, "x": 0, "y": self.y}, "panels": [],
        })
        self.y += 1


def targets(exprs, instant=False):
    out = []
    for i, (expr, legend) in enumerate(exprs):
        out.append({
            "refId": chr(ord("A") + i), "expr": expr, "legendFormat": legend,
            "editorMode": "code", "datasource": DS,
            "instant": instant, "range": not instant,
        })
    return out


def ts(title, exprs, unit="short", desc="", w=8, h=7, stack=False, legend="list", fill=8, min_=0):
    """Time series panel. `exprs` is [(promql, legendFormat), ...]."""
    return {
        "type": "timeseries", "title": title, "description": desc, "datasource": DS,
        "targets": targets(exprs),
        "fieldConfig": {"defaults": {
            "unit": unit, "min": min_,
            "custom": {
                "drawStyle": "line", "lineWidth": 1, "fillOpacity": fill,
                "showPoints": "never", "spanNulls": True,
                "stacking": {"mode": "normal" if stack else "none", "group": "A"},
            },
        }, "overrides": []},
        "options": {
            "legend": {"displayMode": "table" if legend == "table" else "list",
                       "placement": "bottom", "showLegend": legend != "hidden",
                       "calcs": ["lastNotNull", "max"] if legend == "table" else []},
            "tooltip": {"mode": "multi", "sort": "desc"},
        },
    }


def stat(title, expr, unit="short", desc="", w=3, h=4, legend="", decimals=None,
         color="value", thresholds=None, text_size=28):
    steps = thresholds or [{"color": "text", "value": None}]
    return {
        "type": "stat", "title": title, "description": desc, "datasource": DS,
        "targets": targets([(expr, legend)]),
        "fieldConfig": {"defaults": {
            "unit": unit, "decimals": decimals,
            "color": {"mode": "thresholds" if thresholds else "fixed",
                      "fixedColor": "text"},
            "thresholds": {"mode": "absolute", "steps": steps},
        }, "overrides": []},
        "options": {
            "reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
            "colorMode": color, "graphMode": "area", "justifyMode": "auto",
            "textMode": "auto", "text": {"valueSize": text_size},
        },
    }


def node_table(title, columns, desc="", w=24, h=None, link_dashboard=None):
    """One row per node: instant queries joined on the `node` label.

    `columns` is [(column title, promql yielding one series per node, unit)].
    """
    tgts = targets([(expr, "") for _, expr, _ in columns], instant=True)
    for t in tgts:
        t["format"] = "table"
    renames, order, overrides = {"node": "node"}, ["node"], []
    for i, (name, _, unit) in enumerate(columns):
        renames[f"Value #{chr(ord('A') + i)}"] = name
        order.append(name)
        if unit:
            overrides.append({
                "matcher": {"id": "byName", "options": name},
                "properties": [{"id": "unit", "value": unit}],
            })
    if link_dashboard:
        overrides.append({
            "matcher": {"id": "byName", "options": "node"},
            "properties": [{"id": "links", "value": [{
                "title": "Open node dashboard",
                "url": f"/d/{link_dashboard}?var-node=${{__value.text}}&${{__url_time_range}}",
            }]}],
        })
    return {
        "type": "table", "title": title, "description": desc, "datasource": DS,
        "targets": tgts,
        "transformations": [
            {"id": "joinByField", "options": {"byField": "node", "mode": "outer"}},
            {"id": "organize", "options": {
                "renameByName": renames,
                "excludeByName": {"Time": True, "job": True, "instance": True,
                                  "addr": True, "sim": True, "module": True,
                                  "role": True, "__name__": True},
                "indexByName": {n: i for i, n in enumerate(order)},
            }},
        ],
        "fieldConfig": {"defaults": {
            "custom": {"align": "auto", "cellOptions": {"type": "auto"},
                       "filterable": True},
        }, "overrides": overrides},
        "options": {"showHeader": True, "footer": {"show": False},
                    "sortBy": [{"displayName": "node", "desc": False}]},
    }



def heatmap(title, expr, legend, desc="", unit="short", w=12, h=9):
    """Buckets one series per node over time: colour is how many landed in that
    interval, so the spread across nodes and time reads at a glance."""
    return {
        "type": "heatmap", "title": title, "description": desc, "datasource": DS,
        "targets": targets([(expr, legend)]),
        "options": {
            "calculate": True,
            "calculation": {"yBuckets": {"mode": "count", "value": "1"}},
            "color": {"mode": "scheme", "scheme": "Turbo", "steps": 32,
                      "exponent": 0.5, "fill": "dark-orange", "reverse": False},
            "cellGap": 1,
            "yAxis": {"axisPlacement": "left", "unit": unit},
            "legend": {"show": True},
            "tooltip": {"mode": "single", "showColorScale": True, "yHistogram": False},
            "exemplars": {"color": "rgba(255,0,255,0.7)"},
            "filterValues": {"le": 1e-9},
            "rowsFrame": {"layout": "auto"},
            "showValue": "never",
        },
        "fieldConfig": {"defaults": {"custom": {"hideFrom": {
            "tooltip": False, "viz": False, "legend": False}}}, "overrides": []},
    }

def dashboard(uid, title, panels, desc="", templating=None, refresh="5s",
              from_="now-15m", tags=("logos-sim",)):
    return {
        "uid": uid, "title": title, "description": desc, "tags": list(tags),
        "timezone": "browser", "editable": True, "schemaVersion": 39,
        "version": 1, "refresh": refresh, "graphTooltip": 1,
        "time": {"from": from_, "to": "now"},
        "templating": {"list": templating or []},
        "panels": panels,
    }


# `expected` = how many nodes are answering scrapes, i.e. the size of the set a
# member could discover. Used to turn peer counts into a coverage percentage.
EXPECTED = 'scalar(count(up{job="sim-nodes"} == 1))'

# ------------------------------------------------------- discovery overview --

o = Layout()
o.row("Fleet")
o.add(stat("Nodes up", 'count(up{job="sim-nodes"} == 1)',
           desc="Targets answering /metrics: the seed plus every member."), 3, 4)
o.add(stat("Members up", 'count(up{job="sim-nodes", role="member"} == 1)'), 3, 4)
o.add(stat("Discovery coverage (median)",
           f'quantile(0.5, max by (node) (cd_service_table_peers)) / {EXPECTED} '
           f'* 100',
           unit="percent", decimals=0, color="background",
           thresholds=[{"color": "red", "value": None},
                       {"color": "orange", "value": 50},
                       {"color": "green", "value": 90}],
           desc="Median over nodes of the peers held in the service-discovery "
                "tables, as a share of the nodes currently up. It can pass "
                "100% while ads of departed nodes are still cached."), 5, 4)
o.add(stat("Coverage (worst node)",
           f'min(max by (node) (cd_service_table_peers)) / {EXPECTED} * 100',
           unit="percent", decimals=0, color="background",
           thresholds=[{"color": "red", "value": None},
                       {"color": "orange", "value": 50},
                       {"color": "green", "value": 90}]), 5, 4)
o.add(stat("Lookups/s", 'sum(rate(cd_lookup_requests_total[1m]))',
           unit="reqps", decimals=2), 4, 4)
o.add(stat("Peers found per lookup",
           'sum(rate(cd_lookup_peers_found_total[5m])) / '
           'clamp_min(sum(rate(cd_lookup_requests_total[5m])), 0.001)',
           decimals=1,
           desc="Records returned per service lookup, fleet-wide."), 4, 4)

o.row("Discovery convergence")
o.add(ts("Service-discovery peers known, per node",
         [("max by (node) (cd_service_table_peers)", "{{node}}")],
         desc="cd_service_table_peers: peers this node holds across its "
              "service tables. The curve to watch when a run starts.",
         w=12, h=9, legend="table"), 12, 9)
o.add(ts("Kademlia routing-table peers, per node",
         [("max by (node) (kad_routing_table_peers)", "{{node}}")],
         w=12, h=9, legend="table"), 12, 9)

o.add(ts("Registrar occupancy (ads held), per node",
         [("max by (node) (cd_registrar_cache_ads)", "{{node}}")],
         desc="Adverts each node stores as a registrar for others. A node that "
              "stays at 1 is only holding its own.", w=8, h=8), 8, 8)
o.add(ts("Advertiser backlog (pending actions), per node",
         [("max by (node) (cd_advertiser_pending_actions)", "{{node}}")],
         desc="Registrations queued behind a registrar's waiting time. The "
              "anti-sybil term charges ~28 s per shared address-prefix bit, so "
              "clustered addresses show up here.", w=8, h=8), 8, 8)
o.add(ts("Anti-sybil IP tree size, per node",
         [("max by (node) (cd_iptree_total_ips)", "{{node}}")],
         desc="Distinct advertiser addresses in each registrar's prefix tree, "
              "the input to the waiting-time penalty.", w=8, h=8), 8, 8)

o.row("Discovery traffic")
o.add(ts("Register requests vs responses (fleet)",
         [("sum(rate(cd_register_requests_total[1m]))", "requests"),
          ("sum(rate(cd_register_responses_total[1m]))", "responses")],
         unit="reqps", w=8, h=7), 8, 7)
o.add(ts("Service-discovery messages (fleet)",
         [("sum(rate(cd_messages_sent_total[1m]))", "sent"),
          ("sum(rate(cd_messages_received_total[1m]))", "received")],
         unit="reqps", w=8, h=7), 8, 7)
o.add(ts("Service-discovery bytes (fleet)",
         [("sum(rate(cd_message_bytes_sent_total[1m]))", "sent"),
          ("sum(rate(cd_message_bytes_received_total[1m]))", "received")],
         unit="Bps", w=8, h=7), 8, 7)

o.row("Peering")
o.add(ts("Seed: peers admitted (pure libp2p)",
         [('max(logos_delivery_pure_libp2p_peers{role="seed"})', "connected"),
          ('max(logos_delivery_peer_store_size{role="seed"})', "peer store")],
         desc="Members reach the seed as plain libp2p peers; it admits them "
              "against its --max-pure-libp2p-peers budget.", w=8, h=7), 8, 7)
o.add(ts("libp2p peers, per node",
         [('max by (node) (libp2p_peers)', "{{node}}")],
         w=8, h=7), 8, 7)
o.add(ts("Failed dials (fleet)",
         [("sum(rate(libp2p_failed_dials_total[1m]))", "failed dials"),
          ("sum(rate(kad_lookup_undialable_peers_total[1m]))",
           "kad undialable peers")],
         unit="reqps", w=8, h=7), 8, 7)

o.row("Delivery nodes")
o.add(ts("Peers in each node's delivery store",
         [('max by (node) (logos_delivery_peer_store_size'
           '{module=~"delivery_module|in-process"})', "{{node}}")],
         desc="The working set each delivery node actually holds. Discovered "
              "peers appear here only once a backend hands them to the "
              "PeerManager, so this is the line that proves discovery is "
              "feeding the node rather than just succeeding.",
         w=12, h=9, legend="table"), 12, 9)
o.add(ts("Relay mesh peers, per node",
         [('sum by (node) (libp2p_gossipsub_peers_per_topic_mesh'
           '{module=~"delivery_module|in-process"})', "{{node}}")],
         desc="Gossipsub mesh membership, which is what the health monitor "
              "reads to decide whether a node counts as connected.",
         w=12, h=9, legend="table"), 12, 9)

o.row("When peers are discovered")
o.add(heatmap(
    "Discovery arrivals across the fleet",
    'sum by (node) (increase(cd_service_table_insertions_total[30s]))',
    "{{node}}",
    desc="Each cell is one node's newly inserted service-table peers in a "
         "30 s window, so a tight band means the fleet converged together "
         "and a long tail means stragglers waited on registrars.",
    w=12, h=9), 12, 9)
o.add(ts("New peers per minute",
         [('sum(increase(cd_service_table_insertions_total[1m]))',
           "discovery insertions, fleet"),
          ('sum(clamp_min(delta(logos_delivery_peer_store_size'
           '{module=~"delivery_module|in-process"}[1m]), 0))',
           "arrivals in delivery peer stores")],
         desc="The two ends of the same pipe: what discovery inserted, and "
              "what reached the delivery nodes' peer stores.",
         w=12, h=9, fill=25), 12, 9)

o.row("Per node")
o.add(node_table(
    "Node state",
    [("service peers", "max by (node) (cd_service_table_peers)", ""),
     ("coverage", f"max by (node) (cd_service_table_peers) / {EXPECTED} * 100",
      "percent"),
     ("kad peers", "max by (node) (kad_routing_table_peers)", ""),
     ("kad buckets", "max by (node) (kad_routing_table_buckets)", ""),
     ("registrar ads", "max by (node) (cd_registrar_cache_ads)", ""),
     ("advert backlog", "max by (node) (cd_advertiser_pending_actions)", ""),
     ("delivery store", 'max by (node) (logos_delivery_peer_store_size'
      '{module=~"delivery_module|in-process"})', ""),
     ("mesh peers", 'sum by (node) (libp2p_gossipsub_peers_per_topic_mesh'
      '{module=~"delivery_module|in-process"})', ""),
     ("lookups/s", "sum by (node) (rate(cd_lookup_requests_total[5m]))", "reqps"),
     ("found/s", "sum by (node) (rate(cd_lookup_peers_found_total[5m]))", "reqps"),
     ("rss", "sum by (node) (process_resident_memory_bytes)", "bytes"),
     ("cpu", "sum by (node) (rate(process_cpu_seconds_total[1m]))", "percentunit")],
    desc="Click a node name to open its dashboard.",
    link_dashboard="logos-sim-node", h=12), 24, 12)

overview = dashboard(
    "logos-sim-discovery", "Logos sim — discovery overview", o.panels,
    desc="Fleet view of the compose simulation: how fast and how completely "
         "service discovery converges, and what the registrars are doing.",
)

# ------------------------------------------------------------ node detail --

# The seed is one process and carries both registries under `module=in-process`;
# a member is two, labelled by the module the series came from. Selecting with
# these keeps each panel to the process that actually owns the metric, and the
# seed still appears in both halves, which is the truth for it.
DELIVERY = '{node="$node", module=~"delivery_module|in-process"}'
LIBP2P = '{node="$node", module=~"libp2p_module|in-process"}'

n = Layout()
node_var = {
    "name": "node", "label": "node", "type": "query", "datasource": DS,
    "query": {"query": 'label_values(up{job="sim-nodes"}, node)', "refId": "A"},
    "definition": 'label_values(up{job="sim-nodes"}, node)',
    "refresh": 2, "sort": 1, "includeAll": False, "multi": False,
    "current": {"text": "seed", "value": "seed"},
}

n.row("Node")
n.add(stat("Up", 'max(up{job="sim-nodes", node="$node"})',
           color="background", text_size=36,
           thresholds=[{"color": "red", "value": None},
                       {"color": "green", "value": 1}]), 3, 4)
n.add(stat("Uptime", 'min(time() - process_start_time_seconds{node="$node"})',
           unit="s", decimals=0), 3, 4)
n.add(stat("RSS", 'sum(process_resident_memory_bytes{node="$node"})',
           unit="bytes",
           desc="Resident memory of the scraped processes: on a member the "
                "delivery and libp2p module hosts, not the whole container."),
      3, 4)
n.add(stat("CPU", 'sum(rate(process_cpu_seconds_total{node="$node"}[1m]))',
           unit="percentunit", decimals=1), 3, 4)
n.add(stat("Peers in the delivery store",
           f'max(logos_delivery_peer_store_size{DELIVERY})',
           color="background",
           desc="The node's own peer store. Everything the node dials comes "
                "from here, whichever backend found it.",
           thresholds=[{"color": "red", "value": None},
                       {"color": "orange", "value": 1},
                       {"color": "green", "value": 3}]), 4, 4)
n.add(stat("Relay mesh peers",
           f'sum(libp2p_gossipsub_peers_per_topic_mesh{DELIVERY})',
           color="background",
           thresholds=[{"color": "red", "value": None},
                       {"color": "orange", "value": 1},
                       {"color": "green", "value": 2}]), 4, 4)
n.add(stat("Service peers known (libp2p)",
           f'max(cd_service_table_peers{LIBP2P})'), 4, 4)

# ---- delivery module --------------------------------------------------------
n.row("Delivery module — health")
n.add(ts("Connectivity, as the health monitor sees it",
         [(f'sum(libp2p_gossipsub_peers_per_topic_mesh{DELIVERY})', "relay mesh peers"),
          (f'max(logos_delivery_pure_libp2p_peers{DELIVERY})', "pure-libp2p peers"),
          (f'sum(libp2p_pubsub_peers{DELIVERY})', "pubsub peers")],
         desc="Connection status is computed from live gossipsub peers: zero "
              "relay peers reads as Disconnected, one or more as at least "
              "PartiallyConnected. The status itself is an event, not a "
              "metric, so these are its inputs rather than the verdict.",
         w=8, h=8), 8, 8)
n.add(ts("Event loop",
         [(f'max(logos_delivery_event_loop_load{DELIVERY})', "load"),
          (f'sum(rate(logos_delivery_event_loop_accumulated_lag_secs_total{DELIVERY}[1m]))',
           "lag accrued per second")],
         desc="Sustained lag means the node is behind on its own work, which "
              "the health monitor treats as unhealthy regardless of peers.",
         unit="percentunit", w=8, h=8), 8, 8)
n.add(ts("Relay mesh health, per topic",
         [(f'sum(libp2p_gossipsub_healthy_peers_topics{DELIVERY})', "healthy topics"),
          (f'sum(libp2p_gossipsub_low_peers_topics{DELIVERY})', "topics below target"),
          (f'sum(libp2p_gossipsub_no_peers_topics{DELIVERY})', "topics with no peers")],
         desc="A topic with no peers cannot deliver, whatever the totals say.",
         w=8, h=8), 8, 8)

n.row("Delivery module — peers")
n.add(ts("Peers the node holds",
         [(f'max(logos_delivery_peer_store_size{DELIVERY})', "peer store"),
          (f'max(logos_delivery_total_unique_peers{DELIVERY})', "unique seen"),
          (f'max(logos_delivery_pure_libp2p_peers{DELIVERY})', "pure libp2p")],
         desc="Peer store is the working set. Discovered peers reach it only "
              "because each backend writes them there.",
         w=8, h=8), 8, 8)
n.add(ts("The delivery switch's own dialling",
         [(f'sum(libp2p_peers{DELIVERY})', "connected peers"),
          (f'sum(rate(logos_delivery_node_conns_initiated_total{DELIVERY}[1m]))',
           "dials started/s"),
          (f'sum(rate(libp2p_connections_opened_total{DELIVERY}[1m]))',
           "connections opened/s"),
          (f'sum(rate(libp2p_failed_dials_total{DELIVERY}[1m]))',
           "failed dials/s")],
         desc="What the node does with the peers it was handed: the per-"
              "protocol gauges stay empty here because discovered peers carry "
              "advertised service ids, not waku codecs, so this shows the "
              "switch's own activity instead.",
         w=8, h=8), 8, 8)
n.add(ts("New peers arriving in the delivery store",
         [(f'clamp_min(delta(logos_delivery_peer_store_size{DELIVERY}[1m]), 0)',
           "peers added per minute")],
         desc="The arrival signal: peers found by discovery only show here "
              "once a backend hands them to the PeerManager.",
         w=8, h=8, fill=40), 8, 8)

# ---- libp2p module ----------------------------------------------------------
n.row("libp2p module — service discovery")
n.add(ts("Peers and services known",
         [(f"max(cd_service_table_peers{LIBP2P})", "service-table peers"),
          (f"max(cd_service_tables_count{LIBP2P})", "service tables"),
          (f"sum(rate(cd_service_table_insertions_total{LIBP2P}[1m]))",
           "insertions/s")], w=8, h=8), 8, 8)
n.add(ts("Lookups",
         [(f"sum(rate(cd_lookup_requests_total{LIBP2P}[1m]))", "lookups/s"),
          (f"sum(rate(cd_lookup_peers_found_total{LIBP2P}[1m]))",
           "peers found/s")], unit="reqps", w=8, h=8), 8, 8)
n.add(ts("Advertising and registration",
         [(f"max(cd_advertiser_pending_actions{LIBP2P})", "pending actions"),
          (f"sum(rate(cd_register_requests_total{LIBP2P}[1m]))", "requests/s"),
          (f"sum(rate(cd_register_responses_total{LIBP2P}[1m]))", "responses/s")],
         desc="A pending action that will not drain is waiting on a "
              "registrar's waiting time.", w=8, h=8), 8, 8)

n.add(ts("As a registrar",
         [(f"max(cd_registrar_cache_ads{LIBP2P})", "ads held"),
          (f"max(cd_registrar_cache_services{LIBP2P})", "services held"),
          (f"max(cd_iptree_total_ips{LIBP2P})", "ips in prefix tree")],
         w=8, h=8), 8, 8)
n.add(ts("Discovery messages and latency",
         [(f"sum(rate(cd_messages_sent_total{LIBP2P}[1m]))", "sent/s"),
          (f"sum(rate(cd_messages_received_total{LIBP2P}[1m]))", "received/s"),
          (f"histogram_quantile(0.95, sum by (le) "
           f"(rate(cd_message_duration_ms_bucket{LIBP2P}[5m]))) / 1000",
           "p95 duration")], w=8, h=8), 8, 8)
n.add(ts("Discovery bytes",
         [(f"sum(rate(cd_message_bytes_sent_total{LIBP2P}[1m]))", "sent"),
          (f"sum(rate(cd_message_bytes_received_total{LIBP2P}[1m]))", "received")],
         unit="Bps", w=8, h=8), 8, 8)

n.row("libp2p module — kademlia and connections")
n.add(ts("Routing table",
         [(f"max(kad_routing_table_peers{LIBP2P})", "peers"),
          (f"max(kad_routing_table_buckets{LIBP2P})", "buckets"),
          (f"max(kad_network_size_estimate{LIBP2P})", "network size estimate")],
         w=8, h=8), 8, 8)
n.add(ts("Routing table churn",
         [(f"sum(rate(kad_routing_table_insertions_total{LIBP2P}[1m]))",
           "insertions/s"),
          (f"sum(rate(kad_routing_table_replacements_total{LIBP2P}[1m]))",
           "replacements/s"),
          (f"sum(rate(kad_lookup_undialable_peers_total{LIBP2P}[1m]))",
           "undialable/s")], unit="reqps", w=8, h=8), 8, 8)
n.add(ts("libp2p connections, both processes",
         [('max by (module) (libp2p_peers{node="$node"})', "{{module}} peers"),
          (f'sum(rate(libp2p_failed_dials_total{LIBP2P}[1m]))',
           "libp2p module failed dials/s")],
         desc="Two switches on a member: the delivery node's own, and the one "
              "inside libp2p_module that serves discovery.",
         w=8, h=8), 8, 8)

node = dashboard(
    "logos-sim-node", "Logos sim — node detail", n.panels,
    desc="One simulation node in detail, split by the process that owns each "
         "metric: the delivery node's own health, peers and relay mesh, then "
         "the libp2p module that hosts discovery for it.",
    templating=[node_var],
)

# ------------------------------------------------------------------ write --

os.makedirs(OUT, exist_ok=True)
for name, dash in (("discovery-overview", overview), ("node-detail", node)):
    path = os.path.join(OUT, f"{name}.json")
    with open(path, "w") as f:
        json.dump(dash, f, indent=2)
        f.write("\n")
    print(f"{path}: {len([p for p in dash['panels'] if p['type'] != 'row'])} panels")
