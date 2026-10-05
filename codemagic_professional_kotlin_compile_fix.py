#!/usr/bin/env python3
from pathlib import Path
import re
import subprocess
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

if "breakText(" in s:
    raise SystemExit("Kotlin compatibility hazard remains: Paint.breakText overload")

# The interaction/performance layer upgraded the four TONE callbacks to
# (index, value, final). The professional polish must preserve that contract.
tone_sig2 = "commit: (Int, Float) -> Unit)"
tone_sig3 = "commit: (Int, Float, Boolean) -> Unit)"
tone_start = s.find("private fun drawToneSection(")
if tone_start < 0:
    raise SystemExit("drawToneSection missing during Kotlin compatibility fix")
tone_end = s.find("\n    private fun drawUpperFx(", tone_start)
if tone_end < 0:
    raise SystemExit("cannot bound drawToneSection during Kotlin compatibility fix")
tone = s[tone_start:tone_end]
if tone_sig2 in tone:
    tone = tone.replace(tone_sig2, tone_sig3, 1)
elif tone_sig3 not in tone:
    raise SystemExit("drawToneSection callback contract is neither Function2 nor Function3")

tone = tone.replace("commit(i, nv)", "commit(i, nv, true)")
tone = tone.replace("commit(i, it)", "commit(i, it, true)")
tone = tone.replace("commit(i, default)", "commit(i, default, true)")
if tone_sig3 not in tone:
    raise SystemExit("drawToneSection Function3 contract was not installed")
if re.search(r"\bcommit\s*\(\s*i\s*,\s*(?:nv|it|default)\s*\)", tone):
    raise SystemExit("stale Function2 tone commit invocation remains")
s = s[:tone_start] + tone + s[tone_end:]

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
    "commit: (Int, Float, Boolean) -> Unit)",
]:
    if required not in s:
        raise SystemExit(f"professional UI contract lost during Kotlin compile fix: {required}")

view.write_text(s, encoding="utf-8")
print(f"PASS: professional UI Kotlin compatibility normalized; TONE Function3/final contract preserved (breakText fixes: {replaced_break_text})")

# This runs after the complete GUI/bank patch chain, so latency changes cannot be
# overwritten by earlier stability patches. It deliberately preserves ALSA 48k/96x3
# and the existing RT/root safety topology.
latency_patch = Path(__file__).resolve().parent / "codemagic_live_note_latency_fix.py"
if not latency_patch.is_file():
    raise SystemExit(f"missing final live-note latency patch: {latency_patch}")
subprocess.run([sys.executable, str(latency_patch), str(root)], check=True)
