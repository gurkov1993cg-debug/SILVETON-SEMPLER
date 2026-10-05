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

def ensure_import(text: str, anchor: str, statement: str) -> str:
    if statement in text:
        return text
    if anchor not in text:
        raise SystemExit(f"missing import anchor for {statement}")
    return text.replace(anchor, anchor + statement + "\n", 1)

v = ensure_import(v, "import android.app.AlertDialog\n", "import android.app.Dialog")
v = ensure_import(v, "import android.graphics.Color\n", "import android.graphics.drawable.GradientDrawable")
v = ensure_import(v, "import android.view.GestureDetector\n", "import android.view.Gravity")
v = ensure_import(v, "import android.view.Gravity\n", "import android.view.Window")
v = ensure_import(v, "import android.view.Window\n", "import android.view.WindowManager")
v = ensure_import(v, "import android.widget.EditText\n", "import android.widget.Button")

required_view = [
    "override fun onLongPress(e: MotionEvent)",
    "val longPress: ((Float, Float) -> Unit)? = null",
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

view.write_text(v, encoding="utf-8")
print("PASS: professional LIST/parameter UI compile imports + contracts verified")
