#!/bin/bash
# Relaunch RVGL VR with a scripted-input file and take headset screenshots at given times.
#   tools/script_run.sh "<selftest lines; separated>" "<shot times in s after launch, space separated>" <total_s>
ADB="timeout 60 /home/bilko/workspace/android-sdk/platform-tools/adb -s ${QUEST:-192.168.178.115:5555}"
cd "$(dirname "$0")/.."
mkdir -p build/shots; rm -f build/shots/*.jpg
printf '%s\n' "$1" | tr ';' '\n' > build/selftest.txt
$ADB push build/selftest.txt /sdcard/RVGL/quest_selftest.txt > /dev/null
$ADB shell am force-stop com.bilko.rvglvr
$ADB shell setprop debug.oculus.guardian_pause 1
$ADB shell am broadcast -a com.oculus.vrpowermanager.prox_close > /dev/null
$ADB shell svc power stayon true
sleep 3
$ADB logcat -c
$ADB shell monkey -p com.bilko.rvglvr -c android.intent.category.LAUNCHER 1 > /dev/null 2>&1
t0=$(date +%s)
for t in $2; do
  while [ $(( $(date +%s) - t0 )) -lt "$t" ]; do sleep 0.5; done
  $ADB shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT > /dev/null 2>&1
done
while [ $(( $(date +%s) - t0 )) -lt "$3" ]; do sleep 1; done
for f in $($ADB shell ls -t /sdcard/Oculus/Screenshots/ | head -20 | tr -d '\r'); do
  ts=$($ADB shell stat -c %Y /sdcard/Oculus/Screenshots/$f | tr -d '\r')
  [ "$ts" -ge "$t0" ] && $ADB pull /sdcard/Oculus/Screenshots/$f build/shots/ > /dev/null
done
$ADB logcat -d > build/run.log
$ADB shell rm -f /sdcard/RVGL/quest_selftest.txt
ls build/shots
uv run -q --with pillow python3 - <<'EOF'
from PIL import Image, ImageDraw
import glob
fs = sorted(glob.glob('build/shots/*.jpg'))
if fs:
    w = 480; h = 300
    S = Image.new('RGB', (min(len(fs), 4) * w, ((len(fs) + 3) // 4) * h), 'black')
    d = ImageDraw.Draw(S)
    for i, f in enumerate(fs):
        im = Image.open(f).convert('RGB')
        W, H = im.size
        if W == H:  # one eye square: centre 16:9 crop
            im = im.crop((0, int(H * 0.22), W, int(H * 0.78)))
        im.thumbnail((w, h - 20))
        S.paste(im, ((i % 4) * w, (i // 4) * h + 20))
        d.text(((i % 4) * w + 4, (i // 4) * h + 4), str(i + 1) + ' ' + f[-10:], fill='yellow')
    S.save('build/sheet.png')
    print('sheet', len(fs))
EOF
