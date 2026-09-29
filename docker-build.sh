#!/bin/sh
# Build and test the x86-64 Linux shared object from any Docker host,
# including Apple Silicon Macs (the container runs under amd64 emulation).
# Output: build-docker/linux_native_camera_tweaks.so
set -eu

cd "$(dirname "$0")"
IMAGE=lnct-build

docker build -q --platform linux/amd64 -t "$IMAGE" - >/dev/null <<'EOF'
FROM ubuntu:22.04
RUN apt-get update -qq \
	&& DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
		build-essential cmake libsdl2-dev >/dev/null \
	&& rm -rf /var/lib/apt/lists/*
EOF

mkdir -p build-docker
docker run --rm --platform linux/amd64 \
	-v "$PWD":/src:ro -v "$PWD/build-docker":/out \
	"$IMAGE" sh -ec '
		cmake -S /src -B /tmp/build -DBUILD_TESTING=ON >/dev/null
		cmake --build /tmp/build -j"$(nproc)"
		ctest --test-dir /tmp/build --output-on-failure
		cp /tmp/build/liblinux_native_camera_tweaks.so /out/linux_native_camera_tweaks.so
	'

echo "Built build-docker/linux_native_camera_tweaks.so"
