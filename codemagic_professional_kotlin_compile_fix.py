#!/usr/bin/env python3
from pathlib import Path
import re
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
s = re.sub(
    r"if\s*\(\s*maxWidth\.isFinite\(\)\s*&&\s*maxWidth\s*>\s*1f\s*\)\s*\{",
    "if(maxWidth != Float.POSITIVE_INFINITY && maxWidth>1f){",
    s,
)

# Paint.breakText has Java overload/nullability ambiguity at the Kotlin boundary.
# Replace any spacing variant structurally, not by one exact source string.
lines = s.splitlines(keepends=True)
out = []
i = 0
replaced_break_text = 0
while i < len(lines):
    line = lines[i]
    compact = "".join(line.split())
    if "paint.breakText(shown,true,room,null)" in compact:
        indent = line[:len(line) - len(line.lstrip())]
        out.append(f"{indent}var count=shown.length\n")
        out.append(f"{indent}while(count>0 && paint.measureText(shown,0,count)>room){{ count-- }}\n")
        # The generated helper places the old 'shown=if(count...)' assignment on
        # the following line. Remove it independent of whitespace formatting.
        if i + 1 < len(lines):
            next_compact = "".join(lines[i + 1].split())
            if next_compact.startswith("shown=if(count<=0)ell"):
                i += 1
        out.append(f"{indent}shown=if(count<=0)ell else shown.take(count)+ell\n")
        replaced_break_text += 1
    else:
        out.append(line)
    i += 1
s = "".join(out)

# If breakText appeared, every occurrence must have been normalized.
if "breakText(" in s:
    raise SystemExit("Kotlin compatibility hazard remains: Paint.breakText overload")

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
print(f"PASS: professional UI Kotlin compatibility normalized without touching ALSA/MIDI (breakText fixes: {replaced_break_text})")
