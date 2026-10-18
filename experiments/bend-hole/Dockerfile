FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    calculix-ccx=2.21-1 \
    python3-gmsh=4.12.1+ds1-1.1build2 \
    python3-numpy python3-matplotlib ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV OMP_NUM_THREADS=2 CCX_NPROC_RESULTS=2 MPLBACKEND=Agg
WORKDIR /workspace
