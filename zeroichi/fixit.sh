#!/bin/bash
# Zero Ichi repair + report script. Runs ONCE at boot when added to cmdline.txt,
# then removes itself from cmdline.txt and reboots. Report: fixit-report.txt
REPORT=/boot/firmware/zeroichi/fixit-report.txt
exec >"$REPORT" 2>&1

# 1) undo the cmdline.txt change first, so this never runs twice
sed -i -e 's| *systemd\.run=[^ ]*||g' \
       -e 's| *systemd\.run_success_action=[^ ]*||g' \
       -e 's| *systemd\.unit=kernel-command-line\.target||g' /boot/firmware/cmdline.txt
echo "cmdline.txt now:"; cat /boot/firmware/cmdline.txt; echo

echo "===== system ====="; date; uname -a; cat /proc/device-tree/model; echo
grep PRETTY_NAME /etc/os-release
vcgencmd get_throttled
echo "===== before repair ====="
ls -l /dev/spidev* 2>&1
grep -n -i -E 'spi|camera' /boot/firmware/config.txt
systemctl is-enabled zeroichi 2>&1
ls -l /etc/systemd/system/zeroichi.service 2>&1

echo "===== repair ====="
mkdir -p /boot/firmware/photos
grep -q '^dtparam=spi=on' /boot/firmware/config.txt || {
    raspi-config nonint do_spi 0 2>&1
    grep -q '^dtparam=spi=on' /boot/firmware/config.txt || printf '\n[all]\ndtparam=spi=on\n' >> /boot/firmware/config.txt
}
cat > /etc/systemd/system/zeroichi.service <<'UNIT'
[Unit]
Description=Zero Ichi toy camera
RequiresMountsFor=/boot/firmware
After=local-fs.target

[Service]
Environment=PYTHONDONTWRITEBYTECODE=1
WorkingDirectory=/boot/firmware/zeroichi
ExecStart=/usr/bin/python3 /boot/firmware/zeroichi/zeroichi.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT
systemctl enable zeroichi.service 2>&1
# (v53) the old name's service, if this card still has it (Zero Ichi was ToyCam)
systemctl disable toycam.service >/dev/null 2>&1; rm -f /etc/systemd/system/toycam.service
echo "spi line:"; grep -n spi /boot/firmware/config.txt

echo "===== python packages ====="
for m in numpy PIL gpiozero lgpio spidev libcamera picamera2; do
    printf '%-10s ' "$m"; python3 -c "import $m; print('OK', getattr($m,'__version__',''))" 2>&1 | tail -1
done
echo "===== zeroichi log from previous boot ====="
journalctl -u zeroichi -b -1 -n 40 --no-pager 2>&1
echo "===== cloud-init ====="
cloud-init status --long 2>&1
echo "===== done ====="
sync
exit 0
