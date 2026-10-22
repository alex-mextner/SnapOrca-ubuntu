#!/usr/bin/env bash
# Builds and tests the current checkout (main tree or worktree) on the build server, not here.
#
#   scripts/ubuntu2604/server-build.sh [--name NAME] [--no-build] [--cmd 'SHELL CMD'] [CTEST_ARG...]
#
# rsyncs the tree to ~/work/NAME on the server (NAME defaults to the checkout's directory name;
# build/ and deps/build/ stay on the server between runs, so rebuilds are incremental), installs the
# rootless toolchain if missing (server-env.sh), then under `flock ~/build.lock` (one heavy job at a
# time: the server has 16 cores) runs ./build_linux.sh -dstr, builds all targets and runs ctest, logging
# to ~/work/NAME.log. CTEST_ARGs go to ctest (e.g. -R 'CGAL repair'). --no-build skips the build and
# only runs ctest; --cmd runs SHELL CMD in the tree on the server (toolchain env sourced) instead of the build+ctest sequence, still under the lock. SERVER=user@host overrides the host.
# The rsync happens before the lock: use a distinct --name for every concurrent run.
# Exit codes: 11 build failed, 12 tests failed, other non-zero: ssh/rsync errors.
set -euo pipefail

SERVER=${SERVER:-ultra@65.21.40.51}
HERE=$(dirname "$(readlink -f "$0")")
ROOT=$(git -C "$HERE" rev-parse --show-toplevel)
name=$(basename "$ROOT") build=1 cmd=""
while [[ ${1:-} == --* ]]; do
    case $1 in
        --name) name=$2; shift 2 ;;
        --no-build) build=0; shift ;;
        --cmd) cmd=$2; shift 2 ;;
        *) break ;; # everything else is a ctest argument
    esac
done
ctest_args=""
(($# == 0)) || ctest_args=$(printf ' %q' "$@")

ssh "$SERVER" 'mkdir -p ~/work/env ~/work/'"$(printf %q "$name")"
rsync -a --delete --compress --info=stats0 \
    --exclude='/.git' --exclude='/.worktrees' --exclude='/build' --exclude='/deps/build' --exclude='/deps/DL_CACHE' --exclude='/.omp' \
    "$ROOT/" "$SERVER:work/$name/"
rsync -a "$HERE/server-env.sh" "$HERE/server-run.sh" "$SERVER:work/env/"

# shellcheck disable=SC2029
ssh "$SERVER" "set -o pipefail; bash ~/work/env/server-run.sh $(printf '%q ' "$name" "$build" "$cmd" "$ctest_args") 2>&1 | tee ~/work/$(printf %q "$name").log"
