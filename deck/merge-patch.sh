#!/bin/bash
# Apply a patch from the friend onto our tree, the way it actually works here.
#
#   merge-patch.sh /home/deck/Downloads/00-all-changes\(1\).patch [branch]
#
# Our tree is upstream main + our Deck commits, theirs is another base, so a plain `git apply`
# fails on nearly every file. This: strips the CRLF the patches come with (LF in, LF out), applies
# three-way, and then checks for the five things that have broken every merge so far, each of which
# compiles on their side and not on ours.
set -u
P=${1:?usage: merge-patch.sh <patch> [branch]}
BR=${2:-friend-$(date +%m%d-%H%M)}
cd "$(dirname "$0")/.." || exit 1

[ -z "$(git status --porcelain)" ] || { echo "working tree is not clean; commit or stash first"; exit 1; }

LF=/tmp/opencode/patch-lf-$$.patch
tr -d '\r' < "$P" > "$LF"
echo "== files in the patch: $(grep -c '^diff --git' "$LF")"
echo "== CRLF stripped"

git checkout -q -b "$BR" || exit 1
git apply --3way "$LF" 2>&1 | sed 's/^/   /'

echo
echo "== conflicts:"
U=$(git diff --name-only --diff-filter=U)
if [ -n "$U" ]; then
    for f in $U; do printf "   %-28s %s markers\n" "$f" "$(grep -c '^<<<<<<<' "$f")"; done
else
    echo "   none"
fi

echo
echo "== the five that have broken every merge (check each, they compile on their side):"
echo -n "   1. _Alignas on a struct tag (illegal in C): "
if grep -rn 'typedef struct _Alignas\|struct _Alignas' --include=*.c --include=*.h . 2>/dev/null | head -3 | grep -q .; then
    echo "FOUND"; grep -rn 'typedef struct _Alignas\|struct _Alignas' --include=*.c --include=*.h . | head -3 | sed 's/^/      /'
else echo "ok"; fi

echo -n "   2. Windows memory API in host/ (POSIX needs mprotect/calloc): "
if grep -rln 'VirtualProtect\|VirtualAlloc' host/*.c 2>/dev/null | head -3 | grep -q .; then
    echo "CHECK"; grep -rn 'VirtualProtect\|VirtualAlloc' host/*.c | head -4 | sed 's/^/      /'
else echo "ok"; fi

echo -n "   3. SRWLOCK outside _WIN32: "
if grep -rn 'SRWLOCK' host/hos_mem.c host/nn_*.c 2>/dev/null | head -3 | grep -q .; then
    echo "CHECK"; grep -rn 'SRWLOCK' host/hos_mem.c host/nn_*.c 2>/dev/null | head -4 | sed 's/^/      /'
else echo "ok"; fi

echo -n "   4. a new path-carrying HOS_/RX_ switch (hookshot_env.txt refuses paths): "
if grep -rn 'HOS_SAMPLE_ALL\|RX_VK_FRAMELOG\|HOS_WATCH' host/ gpu/ 2>/dev/null | head -2 | grep -q .; then
    echo "note: those belong in the launcher's exports, not the env file"
else echo "ok"; fi

echo -n "   5. nx2/ or a64.h touched (that rebuilds all 80 lifted chunks, ~30 min): "
if git diff --name-only | grep -qE '^(nx2/|third_party/a64-kit/rt/a64\.h)'; then
    echo "YES — expect a full rebuild"; git diff --name-only | grep -E '^(nx2/|third_party/a64-kit/rt/a64\.h)' | sed 's/^/      /'
else echo "no — host files only, ~15 s incremental"; fi

echo
echo "== then: build with /home/deck/hs/build-rebase.sh on white-dragon."
echo "   (three host files have needed their own POSIX fix each time: see deck/PROFILE-2026-11-13.md)"
