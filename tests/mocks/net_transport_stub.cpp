// Integration tests run the default path only: no provider to reach.
#include "../../src/net_bridge.h"

std::shared_ptr<Libp2pTransport> makeLibp2pTransport(const LogosModuleContext& /*context*/,
                                                     const std::string& /*provider*/)
{
    return nullptr;
}
