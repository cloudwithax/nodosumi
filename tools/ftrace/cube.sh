#!/bin/sh
# cube.sh shot [out.png]   grab the Cube's framebuffer, both pages side by side
# cube.sh bot "<line>"...  append lines to the running bot script
set -e
SSH="sshpass -p tntlove4 ssh root@192.168.1.195"
case "$1" in
shot)
  $SSH 'cat /dev/fb0' | python3 -c "
import sys
from PIL import Image
d = sys.stdin.buffer.read()
im = Image.frombuffer('RGBA', (720, 1440), d, 'raw', 'BGRA', 0, 1).convert('RGB')
out = Image.new('RGB', (720, 360))
out.paste(im.crop((0, 0, 720, 720)).resize((360, 360)), (0, 0))
out.paste(im.crop((0, 720, 720, 1440)).resize((360, 360)), (360, 0))
out.save('${2:-/tmp/ct2/shot.png}')
"
  ;;
bot)
  shift
  for l in "$@"; do printf '%s\n' "$l"; done | $SSH 'cat >> /mnt/sdcard/prof/bot.txt'
  ;;
esac
