#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
view = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"
controller = root / "android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt"
for p in (view, controller):
    if not p.is_file():
        raise SystemExit(f"missing LIST UI compile source: {p}")

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
]
for token in required_view:
    if token not in v:
        raise SystemExit(f"professional LIST UI token missing: {token}")

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

print("PASS: current-touch LIST long-press + professional parameter UI contracts verified")
