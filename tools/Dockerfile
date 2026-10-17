FROM ubuntu:24.04
COPY scripts/install-linux-deps.sh /tmp/install-linux-deps.sh
RUN bash /tmp/install-linux-deps.sh && rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
