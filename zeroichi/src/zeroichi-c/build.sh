#!/bin/sh
# Builds zeroichi-c (and zeroichi-lcd) for the Pi Zero (ARMv6, hard float) as
# static programs. (Made with zig 0.17 and libjpeg-turbo 3.1.)
#   needs: zig (as the cross compiler) and libjpeg-turbo built the same way:
#     cmake -DCMAKE_C_COMPILER="zig cc -target arm-linux-musleabihf -mcpu=arm1176jzf_s" \
#           -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm -DWITH_SIMD=0 \
#           -DENABLE_SHARED=0 <libjpeg-turbo source>   (then: make jpeg-static)
#   usage: sh build.sh <libjpeg-turbo source dir> <its build dir>
# On the Pi itself (slow, but works): gcc -O2 -o zeroichi-c *.c -ljpeg -lm -lpthread
set -e
cd "$(dirname "$0")"
J=${1:?libjpeg-turbo source dir}; B=${2:?libjpeg-turbo build dir}
zig cc -target arm-linux-musleabihf -mcpu=arm1176jzf_s -O2 -static -s \
    -I"$J/src" -I"$J" -I"$B" -o zeroichi-c *.c "$B/libjpeg.a" -lm -lpthread
zig cc -target arm-linux-musleabihf -mcpu=arm1176jzf_s -O2 -static -s \
    -o zeroichi-lcd ../zeroichi-lcd.c
echo "built zeroichi-c and zeroichi-lcd - copy both into the zeroichi folder"
