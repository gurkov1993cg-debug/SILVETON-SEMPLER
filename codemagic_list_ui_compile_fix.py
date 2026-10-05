#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
view = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"
controller = root / "android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt"
for p in (view, controller):
    if not p.is_file():
        raise SystemExit(f"missing LIST UI compile source: {p}")

# Normalize SILVETON DSP portamento to the approved 0..100 user scale after all
# older GUI/touch rewrites, so the obsolete 0..5000 ms surface cannot return.
porta = Path(__file__).with_name("codemagic_portamento_100_fix.py")
if not porta.is_file():
    raise SystemExit(f"missing 0..100 portamento patch: {porta}")
exec(compile(porta.read_text(encoding="utf-8"), str(porta), "exec"), {
    "__name__": "__main__", "__file__": str(porta),
})

# Professional control/typography pass plus read-only ALSA/MIDI/root safety audit.
# Older GUI patches are allowed to reformat a Kotlin method signature. Make the
# polish patch resolve such methods by function name as a fallback rather than
# aborting on harmless whitespace/parameter formatting differences.
polish = Path(__file__).with_name("codemagic_professional_polish_final.py")
if not polish.is_file():
    raise SystemExit(f"missing final professional UI/realtime audit patch: {polish}")
polish_source = polish.read_text(encoding="utf-8")
old_method_bounds = '''def method_bounds(text: str, signature: str):
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"missing method: {signature}")
    brace = text.find('{', start)
    if brace < 0:
        raise SystemExit(f"missing method body: {signature}")
    close = match_brace(text, brace)
    if close < 0:
        raise SystemExit(f"unbalanced method: {signature}")
    end = close + 1
    if end < len(text) and text[end] == '\n':
        end += 1
    return start, end
'''
new_method_bounds = '''def method_bounds(text: str, signature: str):
    start = text.find(signature)
    if start < 0 and "fun " in signature:
        fn = signature.split("fun ", 1)[1].split("(", 1)[0].strip()
        for marker in (f"private fun {fn}(", f"fun {fn}("):
            candidate = text.find(marker)
            if candidate >= 0:
                line_start = text.rfind("\n", 0, candidate) + 1
                start = line_start
                break
    if start < 0:
        raise SystemExit(f"missing method: {signature}")
    brace = text.find('{', start)
    if brace < 0:
        raise SystemExit(f"missing method body: {signature}")
    close = match_brace(text, brace)
    if close < 0:
        raise SystemExit(f"unbalanced method: {signature}")
    end = close + 1
    if end < len(text) and text[end] == '\n':
        end += 1
    return start, end
'''
if old_method_bounds not in polish_source:
    raise SystemExit("professional polish method resolver anchor changed")
polish_source = polish_source.replace(old_method_bounds, new_method_bounds, 1)
exec(compile(polish_source, str(polish), "exec"), {
    "__name__": "__main__", "__file__": str(polish),
})

# The seven UPPER rows need the same precision contract as TUNE/TONE/FX: the
# real level and pan bars drag continuously and their value fields allow exact entry.
upper = Path(__file__).with_name("codemagic_upper_row_polish.py")
if not upper.is_file():
    raise SystemExit(f"missing final UPPER-row polish patch: {upper}")
exec(compile(upper.read_text(encoding="utf-8"), str(upper), "exec"), {
    "__name__": "__main__", "__file__": str(upper),
})

# Normalize Kotlin/API edge cases after all professional UI code has been generated.
kotlin_fix = Path(__file__).with_name("codemagic_professional_kotlin_compile_fix.py")
if not kotlin_fix.is_file():
    raise SystemExit(f"missing professional Kotlin compatibility fix: {kotlin_fix}")
exec(compile(kotlin_fix.read_text(encoding="utf-8"), str(kotlin_fix), "exec"), {
    "__name__": "__main__", "__file__": str(kotlin_fix),
})

v = view.read_text(encoding="utf-8")
required_view = [
    "private data class ListLongHit(",
    "private val listLongHits = ArrayList<ListLongHit>",
    "private fun onTouchEventBase(event: MotionEvent): Boolean",
    "ViewConfiguration.getLongPressTimeout()",
    "performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)",
    "showListContextMenu(idx, item)",
    "private fun pasteListClipboard(",
    "private fun showEditorPage(",
    "sampler.renameLiveListEntry(index, newName)",
    "sampler.deleteLiveListEntry(index)",
    "sampler.moveLiveListEntry(",
    "sampler.copyLiveListEntry(clip.item, insertion)",
    "private fun portamentoUiValue(",
    "private fun portamentoSecondsFromUi(",
    "private val portamentoDragRect = RectF()",
    "showNumber(\"PORTAMENTO\", value.toFloat(), 0f, 100f, 0)",
    "portamentoDragActive = true",
    "private data class SmoothSliderHit(",
    "private data class SmoothRangeHit(",
    "handleSmoothControlTouch(event)",
    "registerSmoothSlider(",
    "registerSmoothRange(",
    "private fun drawValueSlider(",
    "import android.widget.SeekBar",
    "SeekBar.OnSeekBarChangeListener",
    "maxWidth:Float=Float.POSITIVE_INFINITY",
    "Typeface.create(\"sans-serif-medium\"",
    "registerSmoothSlider(volBar, -60f, 12f, 0.1f)",
    "registerSmoothSlider(panBar, -1f, 1f, 0.01f)",
    "showNumber(\"UPPER ${i + 1} LEVEL dB\"",
    "showNumber(\"UPPER ${i + 1} PAN\"",
]
for token in required_view:
    if token not in v:
        raise SystemExit(f"final professional sampler UI token missing: {token}")

for forbidden in [
    "MotionEvent.INVALID_POINTER_ID",
    "maxWidth.isFinite()",
    "paint.breakText(shown,true,room,null)",
]:
    if forbidden in v:
        raise SystemExit(f"final Kotlin compatibility hazard remains: {forbidden}")

# Refuse the old portamento presentation and fake-knob regression.
t0 = v.find("    private fun drawTune(c: Canvas, r: RectF, s: SamplerAppController.UpperState)")
t1 = v.find("\n    private fun drawRange(", t0)
if t0 < 0 or t1 < 0:
    raise SystemExit("cannot bound final drawTune")
tune = v[t0:t1]
for stale in ["5000f", "Portamento ms", "s.portamentoSec * 1000f", "drawKnob("]:
    if stale in tune:
        raise SystemExit(f"stale/amateur TUNE control remains: {stale}")

if "GestureDetector missing" in v:
    raise SystemExit("obsolete GestureDetector LIST dependency returned")

c = controller.read_text(encoding="utf-8")
for token in [
    "fun renameLiveListEntry(",
    "fun deleteLiveListEntry(",
    "fun moveLiveListEntry(",
    "fun copyLiveListEntry(",
    "nativeSaveUserPerformanceState",
]:
    if token not in c:
        raise SystemExit(f"professional LIST controller token missing: {token}")

print("PASS: final professional UI + all live sliders precise + Kotlin compatibility + LIST tools + 0..100 portamento + protected ALSA/MIDI audit verified")
