#!/usr/bin/env bash
# Builds and runs fairyfly's portable proxy tests against a real squid. A skipped test (squid missing or not starting)
# fails the run, so the squid cases cannot pass silently.
#   tests/linux_proxy/run.sh            on Ubuntu with the packages below (CI)
#   tests/linux_proxy/run.sh --docker   anywhere with Docker: the same in ubuntu:24.04
# Packages: build-essential cmake ninja-build libssl-dev nlohmann-json3-dev libspdlog-dev catch2 squid openssl
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
if [[ "${1:-}" == "--docker" ]]; then
  exec docker run --rm -v "$root":/src:ro -w /src ubuntu:24.04 bash -c '
    set -e
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null
    apt-get install -y -qq build-essential cmake ninja-build libssl-dev nlohmann-json3-dev libspdlog-dev catch2 squid openssl >/dev/null
    # not as root: squid would switch to its own user and could not read the test directory (as on the CI runner)
    useradd -m tester
    su tester -c "BUILD=/tmp/build tests/linux_proxy/run.sh"'
fi
build="${BUILD:-$root/build-linux-proxy}"
cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$build" --parallel
out="$(mktemp)"
set +e
"$build/linux_proxy_tests" --reporter console 2>&1 | tee "$out"
rc=${PIPESTATUS[0]}
set -e
if grep -q "skipped" "$out" && grep -Eq "[0-9]+ skipped" "$out"; then
  echo "error: a test was skipped (squid not installed or not starting): that is a failure here" >&2
  exit 1
fi
exit "$rc"
