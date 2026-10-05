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


def method_bounds(text: str, signature: str):
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"missing method: {signature}")
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit(f"missing method body: {signature}")
    end = matching(text, brace, "{", "}")
    if end < 0:
        raise SystemExit(f"unbalanced method: {signature}")
    end += 1
    if end < len(text) and text[end] == "\n":
        end += 1
    return start, end

# Dedicated 0..100 performance control state. The displayed number is a stable
# user scale; the DSP continues to receive seconds. A squared time law gives
# much finer useful resolution at short live-performance glide times while
# preserving the existing 0..5 s engine capability at value 100.
if "private val portamentoDragRect = RectF()" not in v:
    anchor = "    private var listPage = 0\n"
    if anchor not in v:
        raise SystemExit("listPage field anchor missing for portamento")
    fields = anchor + '''    private val portamentoDragRect = RectF()
    private var portamentoDragActive = false
    private var lastPortamentoDragValue = -1
'''
    v = v.replace(anchor, fields, 1)

if "private fun portamentoUiValue(" not in v:
    tune_start = v.find("    private fun drawTune(")
    if tune_start < 0:
        raise SystemExit("drawTune missing")
    helpers = r'''    private fun portamentoUiValue(seconds: Float): Float {
        if (seconds <= 0f) return 0f
        val normalized = (seconds / 5f).coerceIn(0f, 1f)
        return (kotlin.math.sqrt(normalized) * 100f).coerceIn(0f, 100f)
    }

    private fun portamentoSecondsFromUi(value: Float): Float {
        val normalized = value.coerceIn(0f, 100f) / 100f
        return 5f * normalized * normalized
    }

    private fun updatePortamentoFromX(x: Float) {
        if (portamentoDragRect.width() <= 1f) return
        val normalized = ((x - portamentoDragRect.left) / portamentoDragRect.width()).coerceIn(0f, 1f)
        val value = kotlin.math.round(normalized * 100f).toInt().coerceIn(0, 100)
        if (value == lastPortamentoDragValue) return
        lastPortamentoDragValue = value
        val state = upperStates[selectedUpper] ?: return
        commitTune(state.copy(portamentoSec = portamentoSecondsFromUi(value.toFloat())))
        invalidate()
    }

'''
    v = v[:tune_start] + helpers + v[tune_start:]

# Replace only the TUNE surface. Other controls and pages remain untouched.
start, end = method_bounds(v, "    private fun drawTune(c: Canvas, r: RectF, s: SamplerAppController.UpperState)")
new_tune = r'''    private fun drawTune(c: Canvas, r: RectF, s: SamplerAppController.UpperState) {
        val gap = 10f.dp
        val cols = 5
        val cw = (r.width() - gap * (cols - 1)) / cols

        // PORTAMENTO: professional 0..100 live slider. Dragging the bar changes
        // one exact unit at a time; tapping the numeric value opens the precise
        // editor page for direct entry. Double-tap on the value resets to 0.
        run {
            val rr = RectF(r.left, r.top, r.left + cw, r.bottom)
            panel(c, rr, C_SUBPANEL, 9f.dp)
            text(c, "PORTAMENTO", rr.centerX(), rr.top + 23f.dp, 10f.dp, C_TEXT_MUTED, alignCenter = true)
            val value = kotlin.math.round(portamentoUiValue(s.portamentoSec)).toInt().coerceIn(0, 100)
            val bar = RectF(rr.left + 24f.dp, rr.centerY() - 8f.dp, rr.right - 24f.dp, rr.centerY() + 8f.dp)
            drawRangeBar(c, bar, 0f, value.toFloat(), 0f, 100f)
            portamentoDragRect.set(bar.left - 8f.dp, bar.top - 18f.dp, bar.right + 8f.dp, bar.bottom + 18f.dp)
            text(c, "0", bar.left, bar.bottom + 22f.dp, 9f.dp, C_TEXT_MUTED)
            text(c, "100", bar.right, bar.bottom + 22f.dp, 9f.dp, C_TEXT_MUTED, alignCenter = true)
            val valueR = RectF(rr.left + 18f.dp, rr.bottom - 48f.dp, rr.right - 18f.dp, rr.bottom)
            text(c, value.toString(), rr.centerX(), rr.bottom - 18f.dp, 14f.dp, C_TEXT, alignCenter = true, bold = true)
            hit(valueR, { _, _ ->
                showNumber("PORTAMENTO", value.toFloat(), 0f, 100f, 0) { exact ->
                    val exactValue = kotlin.math.round(exact).coerceIn(0f, 100f)
                    commitTune(s.copy(portamentoSec = portamentoSecondsFromUi(exactValue)))
                }
            }, { commitTune(s.copy(portamentoSec = 0f)) })
        }

        val labels = listOf(
            Triple("ОКТАВА", s.octave.toFloat(), ""),
            Triple("ТРАНСПОНИРАНЕ", s.transpose.toFloat(), "st"),
            Triple("ФИНА НАСТРОЙКА", s.fineCents, "cent"),
            Triple("PITCH BEND", s.pitchBendRange.toFloat(), "st")
        )
        labels.forEachIndexed { j, item ->
            val i = j + 1
            val rr = RectF(r.left + i * (cw + gap), r.top, r.left + i * (cw + gap) + cw, r.bottom)
            panel(c, rr, C_SUBPANEL, 9f.dp)
            text(c, item.first, rr.centerX(), rr.top + 23f.dp, 10f.dp, C_TEXT_MUTED, alignCenter = true)
            drawKnob(c, rr.centerX(), rr.centerY() + 8f.dp, min(rr.width(), rr.height()) * 0.22f)
            text(c, formatValue(item.second, item.third), rr.centerX(), rr.bottom - 18f.dp, 12f.dp, C_TEXT, alignCenter = true)
            val tap = when (j) {
                0 -> { -> showNumber("Октава", s.octave.toFloat(), -4f, 4f, 0) { commitTune(s.copy(octave = it.toInt())) } }
                1 -> { -> showNumber("Транспониране", s.transpose.toFloat(), -48f, 48f, 0) { commitTune(s.copy(transpose = it.toInt())) } }
                2 -> { -> showNumber("Fine cents", s.fineCents, -100f, 100f, 1) { commitTune(s.copy(fineCents = it)) } }
                else -> { -> showNumber("Pitch Bend Range", s.pitchBendRange.toFloat(), 0f, 24f, 0) { commitTune(s.copy(pitchBendRange = it.toInt())) } }
            }
            val reset = when (j) {
                0 -> { -> commitTune(s.copy(octave = 0)) }
                1 -> { -> commitTune(s.copy(transpose = 0)) }
                2 -> { -> commitTune(s.copy(fineCents = 0f)) }
                else -> { -> commitTune(s.copy(pitchBendRange = 2)) }
            }
            hit(rr, { _, _ -> tap() }, reset)
        }
    }
'''
v = v[:start] + new_tune + v[end:]

# Intercept only the slider bar. The rest of the existing current-touch engine,
# including LIST long-press and all other drag controls, remains untouched.
if "portamentoDragActive = true" not in v:
    sig = "    override fun onTouchEvent(event: MotionEvent): Boolean {"
    touch = v.find(sig)
    if touch < 0:
        raise SystemExit("final onTouchEvent wrapper missing")
    insert_at = touch + len(sig)
    touch_logic = r'''
        if (page == Page.COMBI && tab == UpperTab.TUNE) {
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    if (portamentoDragRect.contains(event.x, event.y)) {
                        portamentoDragActive = true
                        lastPortamentoDragValue = -1
                        parent?.requestDisallowInterceptTouchEvent(true)
                        updatePortamentoFromX(event.x)
                        return true
                    }
                }
                MotionEvent.ACTION_MOVE -> {
                    if (portamentoDragActive) {
                        updatePortamentoFromX(event.x)
                        return true
                    }
                }
                MotionEvent.ACTION_UP -> {
                    if (portamentoDragActive) {
                        updatePortamentoFromX(event.x)
                        portamentoDragActive = false
                        parent?.requestDisallowInterceptTouchEvent(false)
                        return true
                    }
                }
                MotionEvent.ACTION_CANCEL -> {
                    if (portamentoDragActive) {
                        portamentoDragActive = false
                        parent?.requestDisallowInterceptTouchEvent(false)
                        return true
                    }
                }
            }
        }
'''
    v = v[:insert_at] + touch_logic + v[insert_at:]

# Final contract: no 0..5000 ms UI remains in the TUNE function and the real
# control has both drag and exact-entry paths.
start, end = method_bounds(v, "    private fun drawTune(c: Canvas, r: RectF, s: SamplerAppController.UpperState)")
block = v[start:end]
for forbidden in ['5000f', 'Portamento ms', 's.portamentoSec * 1000f']:
    if forbidden in block:
        raise SystemExit(f"stale portamento UI range remains: {forbidden}")
for token in [
    'showNumber("PORTAMENTO", value.toFloat(), 0f, 100f, 0)',
    'portamentoDragRect.set(',
    'drawRangeBar(c, bar, 0f, value.toFloat(), 0f, 100f)',
    'portamentoSecondsFromUi',
    'portamentoUiValue',
    'portamentoDragActive = true',
    'updatePortamentoFromX(event.x)',
]:
    if token not in v:
        raise SystemExit(f"portamento 0..100 contract missing: {token}")

view.write_text(v, encoding="utf-8")
print("PASS: SILVETON DSP portamento UI normalized to precise smooth 0..100 control")
