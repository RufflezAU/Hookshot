#!/bin/sh
# deck/setup-perf.sh -- one time, run with sudo, on the Deck.
#
#   sudo /home/deck/Games/Hookshot-legacy477b/setup-perf.sh
#
# Why this exists: the CPU runs at ~2.25 GHz of a 3.5 GHz ceiling because the governor is 'powersave' with the energy
# preference 'balance_performance'. The frame is entirely CPU-bound, so that is worth more than every renderer change
# combined -- and there is no unprivileged way to change a cpufreq governor, by design. A game cannot do it and a
# launcher cannot do it; it needs root once.
#
# Two ways are set up here, both standard SteamOS pieces:
#   1. GameMode (installed as standard): the deck user is added to the 'gamemode' group. After that the launcher can
#      call gamemoderun, and gamemoded raises the governor for the game with no password prompt -- the mechanism
#      SteamOS already uses for other games.
#   2. A systemd unit that sets governor=performance and EPP=performance at boot, so the game does not depend on
#      anything at launch. Remove it with: sudo systemctl disable --now hookshot-perf.service
#
# It also lets the game open /dev/udmabuf (step 3). With it the game's memory reaches the GPU as dma-bufs over the game's
# own pages. Without it the memory is imported as host pointers, and amdgpu looks up every page of those again at each
# command submission: about 1 ms a submission on this Deck, several times a frame, inside the frame's replay. The device
# normally belongs to root and the kvm group, which the deck user is not in; a udev rule gives it to the user's own
# group instead, the access the kvm group already has. Where the user could open it already, nothing changes. The game's log says which it got: "mem: guest memory stays
# host-pointer imports" or "vk: dma-bufs of at most ...". Remove it with:
#   sudo rm /etc/udev/rules.d/71-hookshot-udmabuf.rules /etc/modules-load.d/hookshot-udmabuf.conf
#
# /etc on SteamOS survives updates for a while and is reverted by a major OS upgrade; rerun this if it comes back.
set -e
[ "$(id -u)" = 0 ] || { echo "run me with sudo"; exit 1; }
USER_NAME=${SUDO_USER:-deck}

echo "1. game mode group"
if getent group gamemode >/dev/null 2>&1; then
    gpasswd -a "$USER_NAME" gamemode || true
    echo "   $USER_NAME added to 'gamemode' (takes effect at the next login)"
else
    echo "   no 'gamemode' group on this system; skipping"
fi

echo "2. cpu governor at boot"
cat > /etc/systemd/system/hookshot-perf.service <<'EOF'
[Unit]
Description=CPU governor and energy preference for low-latency emulation (Hookshot)
After=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$c" 2>/dev/null || true; done; for c in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do echo performance > "$c" 2>/dev/null || true; done'

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now hookshot-perf.service

echo "3. the game's memory shared with the GPU without a copy (/dev/udmabuf)"
modprobe udmabuf 2>/dev/null || true
if [ -e /dev/udmabuf ]; then
    USER_GROUP=$(id -gn "$USER_NAME")
    printf 'KERNEL=="udmabuf", GROUP="%s", MODE="0660"\n' "$USER_GROUP" > /etc/udev/rules.d/71-hookshot-udmabuf.rules
    # (a loadable module is loaded at boot too; built into the kernel, it is always there)
    if [ -e /sys/module/udmabuf/initstate ]; then echo udmabuf > /etc/modules-load.d/hookshot-udmabuf.conf; fi
    udevadm control --reload 2>/dev/null || true
    # (now, without waiting for the next boot)
    chgrp "$USER_GROUP" /dev/udmabuf && chmod 0660 /dev/udmabuf
    echo "   $(ls -l /dev/udmabuf)"
else
    echo "   no /dev/udmabuf on this kernel; skipping (the game keeps host-pointer imports)"
fi

echo "4. what it is now"
for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do printf '%s ' "$(cat "$c")"; done; echo
echo "   scale (first core): $(( $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) / 1000 )) MHz requested, ceiling $(( $(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq) / 1000 )) MHz"

echo
echo "done. Log out and back in (or reboot) so the group membership applies, then play."
echo "to undo:  sudo systemctl disable --now hookshot-perf.service && sudo gpasswd -d $USER_NAME gamemode"
echo "          sudo rm -f /etc/udev/rules.d/71-hookshot-udmabuf.rules /etc/modules-load.d/hookshot-udmabuf.conf"
