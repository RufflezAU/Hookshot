#!/bin/sh
# legacy + #477 (descriptor cache, tree shadows) + the samples-passed 4096 fix (#487's sunlight flicker)
cd /home/deck/Games/Hookshot-legacy477b || exit 1
CFG=${HOS_CONFIG:-stable}
[ -f "configs/$CFG.env" ] && cp "configs/$CFG.env" hookshot_env.txt
[ -f "configs/$CFG.ini" ] && cp "configs/$CFG.ini" hookshot_options.ini
if [ ! -r /sys/module/udmabuf/parameters/size_limit_mb ] && [ -w /dev/udmabuf ]; then
    export HOS_GUEST_MEMFD=1
fi
[ -f hookshot_log.txt ] && mv -f hookshot_log.txt hookshot_log.txt.old.txt
# No core dump. This process is 495 MB with ~2.5 GB mapped, and SteamOS pipes cores to a Python submitter: a crash while
# closing had the kernel write that out and the desktop was unusable for seconds afterwards. The log keeps the message.
ulimit -c 0 2>/dev/null
exec ./hookshot > hookshot_log.txt 2>&1
