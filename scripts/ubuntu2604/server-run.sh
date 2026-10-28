#!/usr/bin/env bash
# Server half of server-build.sh: runs under ~/build.lock in ~/work/NAME. Not meant to be run by hand.
set -uo pipefail
name=$1 build=$2 cmd=$3 ctest_args=$4
echo "== waiting for ~/build.lock"
exec 9>"$HOME/build.lock"
flock 9
echo "== [$(date +%T)] $name on $(hostname)"
bash "$HOME/work/env/server-env.sh" install || exit 1
# shellcheck disable=SC1091
source "$HOME/work/env/server-env.sh"
cd "$HOME/work/$name" || exit 1
# The server is shared: its /tmp is a 16 GB tmpfs that other sessions fill up (cc1plus then dies with "Disk quota exceeded"), so keep
# compiler temp files on disk, and one cc1plus needs up to ~4 GiB (boost spirit, PCH), so size the parallel jobs by the memory free now.
TMPDIR=$HOME/work/tmp
mkdir -p "$TMPDIR"
export TMPDIR
jobs=$(($(awk '/^MemAvailable:/ {print $2}' /proc/meminfo) / 3500000))
((jobs < 1)) && jobs=1
((jobs > $(nproc))) && jobs=$(nproc)
CMAKE_BUILD_PARALLEL_LEVEL=$jobs
export CMAKE_BUILD_PARALLEL_LEVEL
if [[ -n $cmd ]]; then
    eval "$cmd"
    exit $?
fi
if [[ $build == 1 ]]; then
    ./build_linux.sh -dstr || exit 11
    cmake --build build --config Release || exit 11
fi
args=()
eval "args=($ctest_args)"
cd build && ctest -C Release -j1 --output-on-failure --no-tests=error "${args[@]}" || exit 12
