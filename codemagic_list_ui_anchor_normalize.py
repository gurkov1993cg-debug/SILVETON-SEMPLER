#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
view = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"
if not view.is_file():
    raise SystemExit(f"missing sampler view: {view}")

v = view.read_text(encoding="utf-8")


def matching(text: str, open_pos: int, open_ch: str, close_ch: str) -> int:
    if open_pos < 0 or open_pos >= len(text) or text[open_pos] != open_ch:
        return -1
    depth = 0
    quote = None
    escape = False
    i = open_pos
    while i < len(text):
        ch = text[i]
        if quote is not None:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == quote:
                quote = None
            i += 1
            continue
        if ch in ('"', "'"):
            quote = ch
            i += 1
            continue
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


# 1) Preserve whatever drag/immediate-touch fields previous GUI patches added to
# Hit and append the long-press callback structurally instead of matching an old
# exact data-class body.
if "val longPress: ((Float, Float) -> Unit)?" not in v:
    hit0 = v.find("private data class Hit(")
    if hit0 < 0:
        raise SystemExit("Hit data class missing")
    p0 = v.find("(", hit0)
    p1 = matching(v, p0, "(", ")")
    if p1 < 0:
        raise SystemExit("Hit data class parentheses are unbalanced")
    before = v[:p1].rstrip()
    separator = "" if before.endswith(",") else ","
    insert = separator + "\n        val longPress: ((Float, Float) -> Unit)? = null\n    "
    v = before + insert + v[p1:]

# 2) Extend the existing hit() helper without throwing away drag callbacks that
# may already be present. Add a named constructor argument so field order does
# not matter.
if "longPress:((Float,Float)->Unit)?=null" not in v:
    h0 = v.find("private fun hit(")
    if h0 < 0:
        raise SystemExit("hit() helper missing")
    hp0 = v.find("(", h0)
    hp1 = matching(v, hp0, "(", ")")
    if hp1 < 0:
        raise SystemExit("hit() parameter list is unbalanced")
    params = v[hp0 + 1:hp1].rstrip()
    sep = "" if params.endswith(",") or not params else ","
    v = v[:hp1] + sep + "longPress:((Float,Float)->Unit)?=null" + v[hp1:]

    # Re-find method after changing its signature and append the callback to the
    # Hit constructor used by this helper.
    h0 = v.find("private fun hit(")
    body0 = v.find("{", h0)
    body1 = matching(v, body0, "{", "}")
    if body0 < 0 or body1 < 0:
        raise SystemExit("hit() body is unbalanced")
    block = v[body0:body1 + 1]
    ctor0 = block.find("Hit(")
    if ctor0 < 0:
        raise SystemExit("hit() no longer constructs Hit")
    ctor_abs = body0 + ctor0
    cp0 = v.find("(", ctor_abs)
    cp1 = matching(v, cp0, "(", ")")
    if cp1 < 0:
        raise SystemExit("Hit constructor call is unbalanced")
    if "longPress = longPress" not in v[cp0:cp1]:
        v = v[:cp1] + ", longPress = longPress" + v[cp1:]

# 3) GestureDetector: inject true Android long-press dispatch after onDoubleTap,
# regardless of extra immediate-touch/drag code around it.
if "override fun onLongPress(e: MotionEvent)" not in v:
    d0 = v.find("private val detector = GestureDetector")
    if d0 < 0:
        raise SystemExit("GestureDetector missing")
    dbl = v.find("override fun onDoubleTap", d0)
    if dbl < 0:
        raise SystemExit("onDoubleTap missing")
    b0 = v.find("{", dbl)
    b1 = matching(v, b0, "{", "}")
    if b0 < 0 or b1 < 0:
        raise SystemExit("onDoubleTap body is unbalanced")
    addition = '''\n        override fun onLongPress(e: MotionEvent) {\n            hitAt(e.x, e.y)?.longPress?.invoke(e.x, e.y)\n            invalidate()\n        }'''
    v = v[:b1 + 1] + addition + v[b1 + 1:]

# 4) Add long-press to the actual populated LIST cell by locating the real
# activateListEntry call inside drawList(), not by relying on formatting.
if "showListContextMenu(idx, item)" not in v:
    dl0 = v.find("private fun drawList(")
    if dl0 < 0:
        raise SystemExit("drawList missing")
    db0 = v.find("{", dl0)
    db1 = matching(v, db0, "{", "}")
    if db0 < 0 or db1 < 0:
        raise SystemExit("drawList body is unbalanced")
    needle = "sampler.activateListEntry(0,0,0,idx)"
    act = v.find(needle, db0, db1)
    if act < 0:
        raise SystemExit("LIST activation call missing")
    call0 = v.rfind("hit(", db0, act)
    if call0 < 0:
        raise SystemExit("LIST row hit() call missing")
    cp0 = v.find("(", call0)
    cp1 = matching(v, cp0, "(", ")")
    if cp1 < 0 or cp1 > db1:
        raise SystemExit("LIST row hit() call is unbalanced")
    v = v[:cp1] + ", longPress = { _, _ -> showListContextMenu(idx, item) }" + v[cp1:]

for token in [
    "val longPress: ((Float, Float) -> Unit)? = null",
    "longPress:((Float,Float)->Unit)?=null",
    "longPress = longPress",
    "override fun onLongPress(e: MotionEvent)",
    "showListContextMenu(idx, item)",
]:
    if token not in v:
        raise SystemExit(f"normalized LIST UI anchor missing: {token}")

view.write_text(v, encoding="utf-8")
print("PASS: LIST UI anchors normalized without discarding existing drag/immediate-touch behavior")
