# Runtime image of the sim: everything is prebuilt by build.sh into out/.
FROM ubuntu:24.04
RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates iproute2 libpq5 && \
    rm -rf /var/lib/apt/lists/*
# logoscore portable bundle (bin/ lib/ modules/) and the modules dir the daemon
# scans: logoscore's built-ins + delivery_module + libp2p_module.
COPY out/logoscore /opt/logoscore
COPY out/modules   /opt/modules
# The seed binary keeps its nix closure (glibc, openssl, ... via rpath); libpq
# is dlopen-ed at run time, hence the apt package above.
COPY out/seed-store/ /nix/store/
COPY out/seed-path   /opt/sim/seed-path
RUN ln -s "$(cat /opt/sim/seed-path)/bin/logosdeliverynode" /usr/local/bin/logosdeliverynode
COPY member.sh            /opt/sim/member.sh
COPY conf/member.json.tpl /opt/sim/member.json.tpl
ENV PATH=/opt/logoscore/bin:$PATH
