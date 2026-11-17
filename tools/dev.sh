#!/usr/bin/env bash
# Runs a command in the pinned tool image with the repository mounted at /work.
#   tools/dev.sh scripts/regress.py
#   tools/dev.sh bash                      # interactive shell
# Image: $TORUS_IMAGE if set, else ghcr.io/<owner>/torus-tools:<tag> where <owner>
# comes from the GitHub origin remote, else a locally built torus-tools:local.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
image="${TORUS_IMAGE:-}"
if [ -z "$image" ]; then
    owner=$(git -C "$root" remote get-url origin 2>/dev/null \
            | sed -nE 's#.*github.com[:/]([^/]+)/.*#\1#p' | tr '[:upper:]' '[:lower:]')
    if [ -n "$owner" ]; then
        image="ghcr.io/${owner}/torus-tools:$("$root/tools/image_tag.sh")"
        docker image inspect "$image" >/dev/null 2>&1 || docker pull -q "$image" >/dev/null 2>&1 || image=""
    fi
    if [ -z "$image" ]; then
        echo "registry image not available; using torus-tools:local" >&2
        image=torus-tools:local
    fi
fi
tty=(); [ -t 0 ] && tty=(-it)
exec docker run --rm "${tty[@]}" -v "$root:/work" -w /work -u "$(id -u):$(id -g)" \
    -e HOME=/tmp -e TORUS_SEED "$image" "$@"
