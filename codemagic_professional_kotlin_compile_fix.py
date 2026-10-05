#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
view = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"
if not view.is_file():
    raise SystemExit(f"missing sampler view: {view}")

s = view.read_text(encoding="utf-8")

# MotionEvent.INVALID_POINTER_ID is not a stable public Kotlin API symbol across
# Android SDK surfaces. Pointer ids use -1 as the invalid sentinel; keep this
# local and dependency-free.
s = s.replace("MotionEvent.INVALID_POINTER_ID", "-1")

# Avoid relying on Float.isFinite() resolution in the generated Android source.
s = s.replace(
    "if(maxWidth.isFinite() && maxWidth>1f){",
    "if(maxWidth != Float.POSITIVE_INFINITY && maxWidth>1f){",
)

# Avoid the Java Paint.breakText nullable-array overload at the Kotlin boundary.
# This slow path runs only for text that still overflows after font shrinking.
old = '''                val count=paint.breakText(shown,true,room,null)\n                shown=if(count<=0)ell else shown.take(count.coerceAtMost(shown.length))+ell\n'''
new = '''                var count=shown.length\n                while(count>0 && paint.measureText(shown,0,count)>room){ count-- }\n                shown=if(count<=0)ell else shown.take(count)+ell\n'''
if old in s:
    s = s.replace(old, new, 1)

# Compile-surface guards for the new professional controls.
for forbidden in [
    "MotionEvent.INVALID_POINTER_ID",
    "maxWidth.isFinite()",
    "paint.breakText(shown,true,room,null)",
]:
    if forbidden in s:
        raise SystemExit(f"Kotlin compatibility hazard remains: {forbidden}")

for required in [
    "private data class SmoothSliderHit(",
    "private data class SmoothRangeHit(",
    "SeekBar.OnSeekBarChangeListener",
    "handleSmoothControlTouch(event)",
    "registerSmoothSlider(",
    "registerSmoothRange(",
    "maxWidth:Float=Float.POSITIVE_INFINITY",
]:
    if required not in s:
        raise SystemExit(f"professional UI contract lost during Kotlin compile fix: {required}")

view.write_text(s, encoding="utf-8")
print("PASS: professional UI Kotlin compatibility normalized without touching ALSA/MIDI")
