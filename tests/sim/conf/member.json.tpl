{
  "entryLayer": "kernel",
  "kernelConf": {
    "cluster-id": 42,
    "shard": [0],
    "relay": true,
    "mix": false,
    "plugin-kad-discovery": true,
    "kad-bootstrap-node": ["@SEED@"],
    "discv5-discovery": false,
    "peer-exchange": false,
    "rendezvous": false,
    "dns-discovery": false,
    "rest": false,
    "metrics-server": false,
    "metrics-logging": false,
    "tcp-port": 44000,
    "nat": "extip:@IP@",
    "kad-service-lookup-interval": @LOOKUP@,
    "kad-random-lookup-interval": @LOOKUP@,
    "log-level": "DEBUG"
  }
}
