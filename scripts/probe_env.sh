#!/usr/bin/env bash
docker version --format 'docker client {{.Client.Version}} server {{.Server.Version}}' 2>&1 | head -2
docker info --format 'cgroup={{.CgroupVersion}} cpus={{.NCPU}} mem={{.MemTotal}}' 2>&1 | head -1
echo "--- can containers use personality(ADDR_NO_RANDOMIZE) (needed by TSan)?"
docker run --rm ubuntu:24.04 setarch x86_64 -R true 2>&1 | tail -2; echo "exit=$?"
echo "--- network"
curl -sI --max-time 10 https://github.com | head -1
curl -sI --max-time 10 https://releases.hashicorp.com | head -1
uname -m
kubectl version --client 2>&1 | head -2
