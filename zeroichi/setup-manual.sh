#!/bin/sh
# Fallback setup, only needed if automatic first-boot setup didn't happen.
# Log in on the Pi (HDMI + keyboard) and run:
#   sudo sh /boot/firmware/zeroichi/setup-manual.sh
set -e
mkdir -p /boot/firmware/photos
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
raspi-config nonint do_spi 0
systemctl daemon-reload
systemctl enable zeroichi.service
python3 -c "import picamera2, numpy, PIL, gpiozero" \
  || echo "WARNING: some Python packages are missing (needs internet: sudo apt install python3-picamera2 python3-gpiozero)"
echo "Done - rebooting in 3 seconds..."
sleep 3
reboot
