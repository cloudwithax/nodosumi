#!/bin/sh
# capture.sh <name> <secs> [fine|off] [perf]
# restarts slot on the Cube with the CT2 bot, records <secs> of driving after
# the state loads, and pulls everything into /tmp/ct2/<name>
set -e
name=$1; secs=$2; mode=$3; perf=$4
SSH="sshpass -p tntlove4 ssh root@192.168.1.195"
L=/mnt/sdcard/System/launch_frontend.sh
$SSH "sed -i '/^export NODOSUMI_FTRACE_FINE/d; s/^#*export NODOSUMI_FTRACE=/export NODOSUMI_FTRACE=/' $L"
[ "$mode" = fine ] && $SSH "sed -i 's#^export NODOSUMI_PERF=1#export NODOSUMI_PERF=1\nexport NODOSUMI_FTRACE_FINE=1#' $L"
[ "$mode" = off ] && $SSH "sed -i 's/^export NODOSUMI_FTRACE=/#export NODOSUMI_FTRACE=/' $L"
$SSH "grep -E '^#?export (NODOSUMI|SLOT)' $L"
scp_to() { sshpass -p tntlove4 scp -O "$1" root@192.168.1.195:"$2"; }
scp_to ~/nodosumi/tools/ftrace/ct2-drive.bot /mnt/sdcard/prof/bot.txt
$SSH 'rm -f /mnt/sdcard/prof/*.bin /mnt/sdcard/prof/*.bin.* /mnt/sdcard/prof/perf.data; sync
  kill $(ps | grep "System/slot" | grep -v core-host | grep -v grep | awk "{print \$1}")
  for i in $(seq 60); do grep -aq "bot load" /mnt/sdcard/Logs/slot.log 2>/dev/null && break; sleep 1; done
  grep -a "bot load" /mnt/sdcard/Logs/slot.log'
if [ "$perf" = perf ]; then
  $SSH "cd /tmp && /mnt/sdcard/System/perf-tools/perf record -k mono -F 999 -a -g -o /mnt/sdcard/prof/perf.data -- sleep $secs 2>&1 | tail -2"
else
  sleep "$secs"
fi
out=/tmp/ct2/$name
mkdir -p "$out"
$SSH 'sync; ls -la /mnt/sdcard/prof'
for f in nodo-trace.bin nodo-trace.bin.names slot-trace.bin slot-trace.bin.names slot-trace.bin.threads; do
  $SSH "[ -f /mnt/sdcard/prof/$f ] && cat /mnt/sdcard/prof/$f" > "$out/$f" || rm -f "$out/$f"
done
if [ "$perf" = perf ]; then
  $SSH "cd /tmp && /mnt/sdcard/System/perf-tools/perf script -i /mnt/sdcard/prof/perf.data -F comm,tid,time,ip,sym,dso 2>/dev/null" > "$out/perf.txt"
  $SSH "cd /tmp && /mnt/sdcard/System/perf-tools/perf report -i /mnt/sdcard/prof/perf.data --stdio --no-children --sort comm,dso,sym -g none 2>/dev/null | head -150" > "$out/perf-report.txt"
fi
$SSH 'grep -a "bot\|nodosumi" /mnt/sdcard/Logs/slot.log | tail -3'
ls -la "$out"
