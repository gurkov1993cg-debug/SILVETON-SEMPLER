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
polish = Path(__file__).with_name("codemagic_professional_polish_final.py")
if not polish.is_file():
    raise SystemExit(f"missing final professional UI/realtime audit patch: {polish}")
exec(compile(polish.read_text(encoding="utf-8"), str(polish), "exec"), {
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

print("PASS: final professional UI + all live sliders precise + LIST tools + 0..100 portamento + protected ALSA/MIDI audit verified")
