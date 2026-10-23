#!/bin/bash
# Build, install and launch RVGL VR on the Quest, record the log and take a headset screenshot.
#   tools/quest_run.sh [seconds] [--no-build] [--no-install]
set -u
cd "$(dirname "$0")/.."
SECS=${1:-40}
SERIAL=${QUEST:-192.168.178.115:5555}
ADB="/home/bilko/workspace/android-sdk/platform-tools/adb -s $SERIAL"
PKG=com.bilko.rvglvr
BUILD=1; INSTALL=1
for a in "$@"; do
  case $a in --no-build) BUILD=0;; --no-install) INSTALL=0;; esac
done
mkdir -p build
if [ $BUILD = 1 ]; then
  JAVA_HOME=$HOME/.local/jdk-17.0.20.1+1 ./gradlew assembleDebug --offline > build/gradle.log 2>&1 || { grep -E 'error|What went wrong' -A5 build/gradle.log | head -40; exit 1; }
  echo "build ok"
fi
top=$(timeout 10 $ADB shell dumpsys activity activities | grep -m1 topResumedActivity)
echo "top: $top"
if [ $INSTALL = 1 ]; then
  timeout 120 $ADB install -r -g build/outputs/apk/debug/RVGL-VR-debug.apk | tail -1
  timeout 10 $ADB shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow
fi
timeout 10 $ADB shell am force-stop $PKG
timeout 10 $ADB shell setprop debug.oculus.guardian_pause 1
timeout 10 $ADB shell am broadcast -a com.oculus.vrpowermanager.prox_close > /dev/null
timeout 10 $ADB shell input keyevent KEYCODE_WAKEUP
timeout 10 $ADB shell svc power stayon true
sleep 3
timeout 10 $ADB logcat -c
timeout $((SECS + 5)) $ADB logcat -v time > build/run.log 2>&1 &
LOGPID=$!
timeout 10 $ADB shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 > /dev/null 2>&1
sleep "$SECS"
timeout 20 $ADB shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT > /dev/null 2>&1
sleep 4
shot=$(timeout 10 $ADB shell ls -t /sdcard/Oculus/Screenshots/ | head -1 | tr -d '\r')
[ -n "$shot" ] && timeout 30 $ADB pull "/sdcard/Oculus/Screenshots/$shot" build/shot.jpg > /dev/null && echo "screenshot: $shot -> build/shot.jpg"
wait $LOGPID 2>/dev/null
echo "pid: $(timeout 10 $ADB shell pidof $PKG)"
echo "wake: $(timeout 10 $ADB shell dumpsys power | grep -m1 mWakefulness=)"
grep -E 'RVGLQuest|F DEBUG|FATAL|Abort|SIGSEGV|backtrace' build/run.log | grep -v 'wrapped gl' | head -60
grep -E "VrApi.*FPS=" build/run.log | tail -2
