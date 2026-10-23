#!/bin/bash
# Poll the SetupActivity status line on the headset (uiautomator) every N seconds.
#   tools/setup_watch.sh <rounds> <interval_s> [tap_x tap_y]
ADB="timeout 60 /home/bilko/workspace/android-sdk/platform-tools/adb -s ${QUEST:-192.168.178.115:5555}"
ROUNDS=${1:-8}; IV=${2:-15}
if [ -n "${3:-}" ]; then $ADB shell input tap "$3" "$4"; fi
for i in $(seq "$ROUNDS"); do
  sleep "$IV"
  $ADB shell uiautomator dump /sdcard/ui.xml > /dev/null 2>&1
  top=$($ADB shell dumpsys activity activities | grep -m1 topResumedActivity | grep -oE 'com\.[^ }]+')
  st=$($ADB shell cat /sdcard/ui.xml | grep -oE 'text="[^"]*(Downloading|Checking|Installing|Done|failed|Step)[^"]*"' | head -1)
  echo "$(date +%T) $top $st"
  case "$top" in *RVGLVRActivity*) break;; esac
done
