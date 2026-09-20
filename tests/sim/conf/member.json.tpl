{
  "entryLayer": "messaging",
  "messagingOverrides": {
    "cluster-id": 42,
    "num-shards-in-network": 1,
    "plugin-kad-discovery": true,
    "kad-bootstrap-node": ["@SEED@"],
    "kad-service-lookup-interval": @LOOKUP@,
    "discv5-discovery": false,
    "tcp-port": 44000,
    "nat": "extip:@IP@",
    "log-level": "DEBUG"
  }
}
