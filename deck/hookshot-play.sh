#!/bin/sh
# Runs Hookshot from the folder this script is in, with the Deck's udmabuf path.
#
#   ./hookshot-play.sh              the "stable" config
#   ./hookshot-play.sh low          configs/low.env + configs/low.ini
#   HOS_CONFIG=fps ./hookshot-play.sh
#   Steam LaunchOptions "low" does the same as the argument.
#
cd "$(dirname "$0")" || exit 1
CFG=${1:-${HOS_CONFIG:-stable}}
if [ -f "configs/$CFG.env" ]; then
    cp "configs/$CFG.env" hookshot_env.txt
else
    echo "hookshot: no configs/$CFG.env (have: $(ls configs/*.env 2>/dev/null | sed 's#configs/##;s#\.env##' | tr '\n' ' '))" >&2
    exit 1
fi
[ -f "configs/$CFG.ini" ] && cp "configs/$CFG.ini" hookshot_options.ini
# The game turns the memfd path on by itself when /sys/module/udmabuf/parameters/size_limit_mb is
# readable. Force it where sysfs is hidden (a Flatpak sandbox) but only if /dev/udmabuf is really
# there: a forced memfd without a working udmabuf has no import at all.
if [ ! -r /sys/module/udmabuf/parameters/size_limit_mb ] && [ -w /dev/udmabuf ]; then
    export HOS_GUEST_MEMFD=1
fi
[ -e /dev/udmabuf ] || echo "hookshot: /dev/udmabuf is missing (sudo mknod /dev/udmabuf c 10 258)" >&2
[ -f hookshot_log.txt ] && mv -f hookshot_log.txt hookshot_log.txt.old.txt
exec ./hookshot "$@" > hookshot_log.txt 2>&1
