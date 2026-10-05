#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
view = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"
controller = root / "android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt"
for p in (view, controller):
    if not p.is_file():
        raise SystemExit(f"missing LIST UI compile source: {p}")

# Apply the final TUNE-page portamento interaction after all previous GUI/touch
# rewrites, so it cannot be overwritten by the older 0..5000 ms surface.
porta = Path(__file__).with_name("codemagic_portamento_100_fix.py")
if not porta.is_file():
    raise SystemExit(f"missing 0..100 portamento patch: {porta}")
exec(compile(porta.read_text(encoding="utf-8"), str(porta), "exec"), {
    "__name__": "__main__",
    "__file__": str(porta),
})

v = view.read_text(encoding="utf-8")
required_view = [
    "private data class ListLongHit(",
    "private val listLongHits = ArrayList<ListLongHit>",
    "private fun listLongHit(",
    "private fun listLongHitAt(",
    "private fun onTouchEventBase(",
    "override fun onTouchEvent(event: MotionEvent): Boolean",
    "ViewConfiguration.getLongPressTimeout()",
    "performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)",
    "listLongHit(rr)",
    "showListContextMenu(idx, item)",
    "private fun pasteListClipboard(",
    "private fun showEditorPage(",
    "GradientDrawable().apply",
    "WindowManager.LayoutParams.FLAG_DIM_BEHIND",
    "Button(context).apply",
    "sampler.renameLiveListEntry(index, newName)",
    "sampler.deleteLiveListEntry(index)",
    "sampler.moveLiveListEntry(",
    "sampler.copyLiveListEntry(clip.item, insertion)",
    "private fun portamentoUiValue(",
    "private fun portamentoSecondsFromUi(",
    "private val portamentoDragRect = RectF()",
    "showNumber(\"PORTAMENTO\", value.toFloat(), 0f, 100f, 0)",
    "drawRangeBar(c, bar, 0f, value.toFloat(), 0f, 100f)",
    "portamentoDragActive = true",
]
for token in required_view:
    if token not in v:
        raise SystemExit(f"professional LIST/portamento UI token missing: {token}")

# The final TUNE page must not expose the old millisecond 0..5000 scale.
t0 = v.find("    private fun drawTune(c: Canvas, r: RectF, s: SamplerAppController.UpperState)")
t1 = v.find("\n    private fun drawRange(", t0)
if t0 < 0 or t1 < 0:
    raise SystemExit("cannot bound final drawTune")
tune = v[t0:t1]
for stale in ["5000f", "Portamento ms", "s.portamentoSec * 1000f"]:
    if stale in tune:
        raise SystemExit(f"stale 0..5000 portamento UI remains: {stale}")

# The new path must not depend on the obsolete GestureDetector contract.
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

print("PASS: current-touch LIST long-press + professional parameter UI + precise 0..100 SILVETON portamento verified")
