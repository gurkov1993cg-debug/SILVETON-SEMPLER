#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
view = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"
bridge = root / "android/app/src/main/cpp/SamplerAudioBridge.cpp"
root_mode = root / "android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt"
runtime = root / "android/app/src/main/java/com/silveton/sampler/SamplerRuntime.kt"
midi = root / "android/app/src/main/java/com/silveton/sampler/SamplerMidiController.kt"
engine = root / "core/src/sampler/SamplerEngine.cpp"
for p in (view, bridge, root_mode, runtime, midi, engine):
    if not p.is_file():
        raise SystemExit(f"missing professional-polish source: {p}")


def match_brace(text: str, open_pos: int) -> int:
    depth = 0
    quote = None
    escape = False
    for i in range(open_pos, len(text)):
        ch = text[i]
        if quote is not None:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == quote:
                quote = None
            continue
        if ch in ('"', "'"):
            quote = ch
            continue
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return i
    return -1


def method_bounds(text: str, signature: str):
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


def replace_method(text: str, signature: str, replacement: str) -> str:
    a, b = method_bounds(text, signature)
    return text[:a] + replacement.rstrip() + "\n" + text[b:]


def ensure_import(text: str, statement: str) -> str:
    if statement in text:
        return text
    imports_end = text.find("\n\n", text.find("import "))
    if imports_end < 0:
        raise SystemExit(f"cannot insert import: {statement}")
    return text[:imports_end] + "\n" + statement + text[imports_end:]

v = view.read_text(encoding="utf-8")
v = ensure_import(v, "import android.widget.SeekBar")

# -----------------------------------------------------------------------------
# One common, pointer-safe smooth-control layer. It sits above the legacy Hit
# layer so sliders respond continuously without changing LIST long-press logic.
# -----------------------------------------------------------------------------
if "private data class SmoothSliderHit(" not in v:
    anchor = "    private val portamentoDragRect = RectF()\n"
    if anchor not in v:
        raise SystemExit("portamento control state missing before final UI polish")
    fields = r'''    private data class SmoothSliderHit(
        val rect: RectF,
        val minValue: Float,
        val maxValue: Float,
        val step: Float,
        val onChange: (Float) -> Unit,
        var lastValue: Float = Float.NaN
    )
    private data class SmoothRangeHit(
        val rect: RectF,
        val minValue: Float,
        val maxValue: Float,
        val step: Float,
        var low: Float,
        var high: Float,
        val onChange: (Float, Float) -> Unit
    )
    private val smoothSliderHits = ArrayList<SmoothSliderHit>(48)
    private val smoothRangeHits = ArrayList<SmoothRangeHit>(4)
    private var activeSmoothSlider: SmoothSliderHit? = null
    private var activeSmoothRange: SmoothRangeHit? = null
    private var activeSmoothRangeHigh = false
    private var smoothPointerId = MotionEvent.INVALID_POINTER_ID
'''
    v = v.replace(anchor, fields + anchor, 1)

if "private fun registerSmoothSlider(" not in v:
    at = v.find("    private fun portamentoUiValue(")
    if at < 0:
        raise SystemExit("portamento helper missing for smooth-control insertion")
    helpers = r'''    private fun quantizeControl(value: Float, minValue: Float, maxValue: Float, step: Float): Float {
        val clamped = value.coerceIn(minValue, maxValue)
        if (step <= 0f) return clamped
        val q = kotlin.math.round((clamped - minValue) / step) * step + minValue
        return q.coerceIn(minValue, maxValue)
    }

    private fun registerSmoothSlider(r: RectF, minValue: Float, maxValue: Float, step: Float, onChange: (Float) -> Unit) {
        smoothSliderHits.add(SmoothSliderHit(RectF(r), minValue, maxValue, step, onChange))
    }

    private fun registerSmoothRange(r: RectF, minValue: Float, maxValue: Float, step: Float, low: Float, high: Float, onChange: (Float, Float) -> Unit) {
        smoothRangeHits.add(SmoothRangeHit(RectF(r), minValue, maxValue, step, low, high, onChange))
    }

    private fun sliderValueAt(hit: SmoothSliderHit, x: Float): Float {
        if (hit.rect.width() <= 1f) return hit.minValue
        val t = ((x - hit.rect.left) / hit.rect.width()).coerceIn(0f, 1f)
        return quantizeControl(hit.minValue + (hit.maxValue - hit.minValue) * t, hit.minValue, hit.maxValue, hit.step)
    }

    private fun emitSmoothSlider(hit: SmoothSliderHit, x: Float) {
        val value = sliderValueAt(hit, x)
        if (hit.lastValue.isNaN() || kotlin.math.abs(value - hit.lastValue) >= max(0.000001f, hit.step * 0.49f)) {
            hit.lastValue = value
            hit.onChange(value)
            invalidate()
        }
    }

    private fun emitSmoothRange(hit: SmoothRangeHit, x: Float) {
        if (hit.rect.width() <= 1f) return
        val t = ((x - hit.rect.left) / hit.rect.width()).coerceIn(0f, 1f)
        val value = quantizeControl(hit.minValue + (hit.maxValue - hit.minValue) * t, hit.minValue, hit.maxValue, hit.step)
        if (activeSmoothRangeHigh) hit.high = max(hit.low, value) else hit.low = min(hit.high, value)
        hit.onChange(hit.low, hit.high)
        invalidate()
    }

    private fun handleSmoothControlTouch(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val slider = smoothSliderHits.asReversed().firstOrNull { it.rect.contains(event.x, event.y) }
                if (slider != null) {
                    activeSmoothSlider = slider
                    smoothPointerId = event.getPointerId(0)
                    parent?.requestDisallowInterceptTouchEvent(true)
                    emitSmoothSlider(slider, event.x)
                    return true
                }
                val range = smoothRangeHits.asReversed().firstOrNull { it.rect.contains(event.x, event.y) }
                if (range != null) {
                    activeSmoothRange = range
                    smoothPointerId = event.getPointerId(0)
                    val lowX = range.rect.left + range.rect.width() * ((range.low - range.minValue) / (range.maxValue - range.minValue)).coerceIn(0f, 1f)
                    val highX = range.rect.left + range.rect.width() * ((range.high - range.minValue) / (range.maxValue - range.minValue)).coerceIn(0f, 1f)
                    activeSmoothRangeHigh = kotlin.math.abs(event.x - highX) < kotlin.math.abs(event.x - lowX)
                    parent?.requestDisallowInterceptTouchEvent(true)
                    emitSmoothRange(range, event.x)
                    return true
                }
            }
            MotionEvent.ACTION_MOVE -> {
                val pointer = smoothPointerId
                if (pointer != MotionEvent.INVALID_POINTER_ID) {
                    val index = event.findPointerIndex(pointer)
                    if (index >= 0) {
                        activeSmoothSlider?.let { emitSmoothSlider(it, event.getX(index)); return true }
                        activeSmoothRange?.let { emitSmoothRange(it, event.getX(index)); return true }
                    }
                }
            }
            MotionEvent.ACTION_UP -> {
                activeSmoothSlider?.let { emitSmoothSlider(it, event.x) }
                activeSmoothRange?.let { emitSmoothRange(it, event.x) }
                if (activeSmoothSlider != null || activeSmoothRange != null) {
                    activeSmoothSlider = null
                    activeSmoothRange = null
                    smoothPointerId = MotionEvent.INVALID_POINTER_ID
                    parent?.requestDisallowInterceptTouchEvent(false)
                    return true
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                if (activeSmoothSlider != null || activeSmoothRange != null) {
                    activeSmoothSlider = null
                    activeSmoothRange = null
                    smoothPointerId = MotionEvent.INVALID_POINTER_ID
                    parent?.requestDisallowInterceptTouchEvent(false)
                    return true
                }
            }
        }
        return false
    }

    private fun drawValueSlider(c: Canvas, r: RectF, value: Float, minValue: Float, maxValue: Float) {
        paint.style = Paint.Style.FILL
        paint.shader = null
        paint.color = C_TRACK
        c.drawRoundRect(r, r.height() * 0.5f, r.height() * 0.5f, paint)
        val t = if (maxValue > minValue) ((value - minValue) / (maxValue - minValue)).coerceIn(0f, 1f) else 0f
        val x = r.left + r.width() * t
        paint.color = C_ACCENT
        if (x > r.left) c.drawRoundRect(RectF(r.left, r.top, x, r.bottom), r.height() * 0.5f, r.height() * 0.5f, paint)
        paint.color = C_KNOB_LIGHT
        c.drawCircle(x, r.centerY(), max(6f.dp, r.height() * 0.68f), paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1f.dp
        paint.color = Color.argb(150, 0, 0, 0)
        c.drawCircle(x, r.centerY(), max(6f.dp, r.height() * 0.68f), paint)
        paint.style = Paint.Style.FILL
    }

'''
    v = v[:at] + helpers + v[at:]

# Rebuild smooth hit maps once per frame beside the existing hit maps.
if "smoothSliderHits.clear()" not in v:
    anchor = "        hits.clear()\n"
    if anchor not in v:
        raise SystemExit("onDraw hit-clear anchor missing")
    v = v.replace(anchor, anchor + "        smoothSliderHits.clear()\n        smoothRangeHits.clear()\n", 1)

# Let smooth controls consume pointer events before portamento and LIST wrappers.
touch_sig = "    override fun onTouchEvent(event: MotionEvent): Boolean {"
touch = v.find(touch_sig)
if touch < 0:
    raise SystemExit("final onTouchEvent missing")
brace = v.find('{', touch)
if "handleSmoothControlTouch(event)" not in v[brace:brace+300]:
    v = v[:brace+1] + "\n        if (handleSmoothControlTouch(event)) return true" + v[brace+1:]

# -----------------------------------------------------------------------------
# TUNE: keep the dedicated 0..100 portamento, replace decorative fake knobs by
# real bars. Numeric value remains a separate exact-entry target.
# -----------------------------------------------------------------------------
new_tune = r'''    private fun drawTune(c: Canvas, r: RectF, s: SamplerAppController.UpperState) {
        val gap = 10f.dp
        val cols = 5
        val cw = (r.width() - gap * (cols - 1)) / cols

        run {
            val rr = RectF(r.left, r.top, r.left + cw, r.bottom)
            panel(c, rr, C_SUBPANEL, 9f.dp)
            text(c, "PORTAMENTO", rr.centerX(), rr.top + 23f.dp, 10f.dp, C_TEXT_MUTED, alignCenter = true, maxWidth = rr.width() - 16f.dp)
            val value = kotlin.math.round(portamentoUiValue(s.portamentoSec)).toInt().coerceIn(0, 100)
            val bar = RectF(rr.left + 24f.dp, rr.centerY() - 7f.dp, rr.right - 24f.dp, rr.centerY() + 7f.dp)
            drawValueSlider(c, bar, value.toFloat(), 0f, 100f)
            portamentoDragRect.set(bar.left - 8f.dp, bar.top - 18f.dp, bar.right + 8f.dp, bar.bottom + 18f.dp)
            val valueR = RectF(rr.left + 18f.dp, rr.bottom - 48f.dp, rr.right - 18f.dp, rr.bottom)
            text(c, value.toString(), rr.centerX(), rr.bottom - 18f.dp, 14f.dp, C_TEXT, alignCenter = true, bold = true, maxWidth = valueR.width())
            hit(valueR, { _, _ ->
                showNumber("PORTAMENTO", value.toFloat(), 0f, 100f, 0) { exact ->
                    commitTune(s.copy(portamentoSec = portamentoSecondsFromUi(kotlin.math.round(exact).coerceIn(0f, 100f))))
                }
            }, { commitTune(s.copy(portamentoSec = 0f)) })
        }

        for (j in 0 until 4) {
            val i = j + 1
            val rr = RectF(r.left + i * (cw + gap), r.top, r.left + i * (cw + gap) + cw, r.bottom)
            panel(c, rr, C_SUBPANEL, 9f.dp)
            val title = when (j) { 0 -> "ОКТАВА"; 1 -> "ТРАНСПОНИРАНЕ"; 2 -> "ФИНА НАСТРОЙКА"; else -> "PITCH BEND" }
            val value = when (j) { 0 -> s.octave.toFloat(); 1 -> s.transpose.toFloat(); 2 -> s.fineCents; else -> s.pitchBendRange.toFloat() }
            val minV = when (j) { 0 -> -4f; 1 -> -48f; 2 -> -100f; else -> 0f }
            val maxV = when (j) { 0 -> 4f; 1 -> 48f; 2 -> 100f; else -> 24f }
            val step = if (j == 2) 0.1f else 1f
            val decimals = if (j == 2) 1 else 0
            text(c, title, rr.centerX(), rr.top + 23f.dp, 10f.dp, C_TEXT_MUTED, alignCenter = true, maxWidth = rr.width() - 16f.dp)
            val bar = RectF(rr.left + 22f.dp, rr.centerY() - 7f.dp, rr.right - 22f.dp, rr.centerY() + 7f.dp)
            drawValueSlider(c, bar, value, minV, maxV)
            registerSmoothSlider(bar, minV, maxV, step) { nv ->
                val current = upperStates[selectedUpper] ?: s
                when (j) {
                    0 -> commitTune(current.copy(octave = nv.toInt()))
                    1 -> commitTune(current.copy(transpose = nv.toInt()))
                    2 -> commitTune(current.copy(fineCents = nv))
                    else -> commitTune(current.copy(pitchBendRange = nv.toInt()))
                }
            }
            val valueR = RectF(rr.left + 12f.dp, rr.bottom - 48f.dp, rr.right - 12f.dp, rr.bottom)
            val display = if (decimals == 0) value.toInt().toString() else "%.1f".format(value)
            text(c, display, rr.centerX(), rr.bottom - 18f.dp, 13f.dp, C_TEXT, alignCenter = true, bold = true, maxWidth = valueR.width())
            hit(valueR, { _, _ ->
                showNumber(title, value, minV, maxV, decimals) { nv ->
                    val current = upperStates[selectedUpper] ?: s
                    when (j) {
                        0 -> commitTune(current.copy(octave = nv.toInt()))
                        1 -> commitTune(current.copy(transpose = nv.toInt()))
                        2 -> commitTune(current.copy(fineCents = nv))
                        else -> commitTune(current.copy(pitchBendRange = nv.toInt()))
                    }
                }
            }, {
                val current = upperStates[selectedUpper] ?: s
                when (j) {
                    0 -> commitTune(current.copy(octave = 0))
                    1 -> commitTune(current.copy(transpose = 0))
                    2 -> commitTune(current.copy(fineCents = 0f))
                    else -> commitTune(current.copy(pitchBendRange = 2))
                }
            })
        }
    }
'''
v = replace_method(v, "    private fun drawTune(c: Canvas, r: RectF, s: SamplerAppController.UpperState)", new_tune)

# VELOCITY RANGE becomes a true two-thumb smooth range without losing exact edit.
new_range = r'''    private fun drawRange(c: Canvas, r: RectF, s: SamplerAppController.UpperState) {
        val rr = RectF(r.left, r.top, r.left + r.width() * 0.62f, r.bottom)
        panel(c, rr, C_SUBPANEL, 9f.dp)
        text(c, "VELOCITY RANGE", rr.left + 16f.dp, rr.top + 25f.dp, 11f.dp, C_TEXT_MUTED, maxWidth = rr.width() - 32f.dp)
        val bar = RectF(rr.left + 38f.dp, rr.centerY() - 7f.dp, rr.right - 38f.dp, rr.centerY() + 7f.dp)
        drawRangeBar(c, bar, s.velocityLow.toFloat(), s.velocityHigh.toFloat(), 1f, 127f)
        registerSmoothRange(bar, 1f, 127f, 1f, s.velocityLow.toFloat(), s.velocityHigh.toFloat()) { lo, hi ->
            val current = upperStates[selectedUpper] ?: s
            commitRange(current.copy(velocityLow = lo.toInt(), velocityHigh = hi.toInt()))
        }
        text(c, s.velocityLow.toString(), rr.left + 18f.dp, rr.centerY() + 5f.dp, 13f.dp, C_TEXT)
        text(c, s.velocityHigh.toString(), rr.right - 18f.dp, rr.centerY() + 5f.dp, 13f.dp, C_TEXT, alignCenter = true)
        hit(RectF(rr.left, rr.top, rr.right, bar.top - 10f.dp), { _, _ -> showVelocityRange(selectedUpper, s) })
        hit(RectF(rr.left, bar.bottom + 10f.dp, rr.right, rr.bottom), { _, _ -> showVelocityRange(selectedUpper, s) }, { commitRange(s.copy(velocityLow = 1, velocityHigh = 127)) })

        val info = RectF(rr.right + 12f.dp, r.top, r.right, r.bottom)
        panel(c, info, C_SUBPANEL, 9f.dp)
        text(c, "KEY RANGE", info.left + 16f.dp, info.top + 25f.dp, 11f.dp, C_TEXT_MUTED, maxWidth = info.width() - 32f.dp)
        text(c, "${noteName(s.keyLow)}  –  ${noteName(s.keyHigh)}", info.left + 16f.dp, info.centerY() + 6f.dp, 22f.dp, C_TEXT, maxWidth = info.width() - 32f.dp)
        text(c, "Докосни за точни граници", info.left + 16f.dp, info.bottom - 19f.dp, 10f.dp, C_TEXT_MUTED, maxWidth = info.width() - 32f.dp)
        hit(info, { _, _ -> showKeyRange(selectedUpper, s) })
    }
'''
v = replace_method(v, "    private fun drawRange(c: Canvas, r: RectF, s: SamplerAppController.UpperState)", new_range)

# TONE: real sliders instead of decorative knobs that looked interactive but were not.
new_tone_section = r'''    private fun drawToneSection(c: Canvas, r: RectF, title: String, values: List<Pair<String, Float>>, commit: (Int, Float) -> Unit) {
        panel(c, r, C_SUBPANEL, 9f.dp)
        text(c, title, r.left + 12f.dp, r.top + 22f.dp, 11f.dp, C_TEXT_MUTED, maxWidth = r.width() - 24f.dp)
        val gap = 5f.dp
        val top = r.top + 32f.dp
        val cellW = (r.width() - gap * (values.size + 1)) / values.size
        values.forEachIndexed { i, pair ->
            val rr = RectF(r.left + gap + i * (cellW + gap), top, r.left + gap + i * (cellW + gap) + cellW, r.bottom - 8f.dp)
            val limits = when (title) {
                "AMP" -> if (i == 2) (0f to 2f) else (0f to 16f)
                "FILTER" -> if (i == 0) (-10f to 10f) else (-1f to 1f)
                "LFO" -> if (i == 0) (0f to 16f) else (0f to 8f)
                else -> (-18f to 18f)
            }
            val step = if (title == "EQ") 0.1f else 0.01f
            val default = when (title) { "AMP" -> 1f; "FILTER" -> 0f; "LFO" -> 1f; else -> 0f }
            text(c, pair.first, rr.centerX(), rr.top + 15f.dp, 9f.dp, C_TEXT_MUTED, alignCenter = true, maxWidth = rr.width() - 8f.dp)
            val bar = RectF(rr.left + 7f.dp, rr.centerY() - 6f.dp, rr.right - 7f.dp, rr.centerY() + 6f.dp)
            drawValueSlider(c, bar, pair.second, limits.first, limits.second)
            registerSmoothSlider(bar, limits.first, limits.second, step) { nv -> commit(i, nv) }
            val valueR = RectF(rr.left, rr.bottom - 34f.dp, rr.right, rr.bottom)
            text(c, "%.2f".format(pair.second), rr.centerX(), rr.bottom - 6f.dp, 10f.dp, C_TEXT, alignCenter = true, bold = true, maxWidth = rr.width() - 6f.dp)
            hit(valueR, { _, _ -> showNumber("$title ${pair.first}", pair.second, limits.first, limits.second, 2) { commit(i, it) } }, { commit(i, default) })
        }
    }
'''
v = replace_method(v, "    private fun drawToneSection(c: Canvas, r: RectF, title: String, values: List<Pair<String, Float>>, commit: (Int, Float) -> Unit)", new_tone_section)

# Upper FX sends are proper smooth sliders with separate exact-value target.
new_upper_fx = r'''    private fun drawUpperFx(c: Canvas, r: RectF, s: SamplerAppController.UpperState) {
        val left = RectF(r.left, r.top, r.left + r.width() * 0.49f, r.bottom)
        val right = RectF(left.right + 10f.dp, r.top, r.right, r.bottom)
        listOf(left to 1, right to 2).forEach { (rr, n) ->
            panel(c, rr, C_SUBPANEL, 9f.dp)
            val value = if (n == 1) s.fxSend1 else s.fxSend2
            text(c, "FX$n SEND", rr.left + 16f.dp, rr.top + 25f.dp, 11f.dp, C_TEXT_MUTED, maxWidth = rr.width() - 32f.dp)
            val bar = RectF(rr.left + 28f.dp, rr.centerY() - 7f.dp, rr.right - 28f.dp, rr.centerY() + 7f.dp)
            drawValueSlider(c, bar, value, 0f, 1f)
            registerSmoothSlider(bar, 0f, 1f, 0.01f) { nv ->
                val current = upperStates[selectedUpper] ?: s
                commitUpperFx(if (n == 1) current.copy(fxSend1 = nv) else current.copy(fxSend2 = nv))
            }
            val valueR = RectF(rr.left + 22f.dp, rr.bottom - 48f.dp, rr.right - 22f.dp, rr.bottom)
            text(c, "${kotlin.math.round(value * 100f).toInt()} %", rr.centerX(), rr.bottom - 18f.dp, 12f.dp, C_TEXT, alignCenter = true, bold = true, maxWidth = valueR.width())
            hit(valueR, { _, _ -> showNumber("FX$n SEND", value * 100f, 0f, 100f, 0) { pct ->
                val nv = (pct / 100f).coerceIn(0f, 1f)
                val current = upperStates[selectedUpper] ?: s
                commitUpperFx(if (n == 1) current.copy(fxSend1 = nv) else current.copy(fxSend2 = nv))
            } }, { val current = upperStates[selectedUpper] ?: s; commitUpperFx(if (n == 1) current.copy(fxSend1 = 0f) else current.copy(fxSend2 = 0f)) })
        }
    }
'''
v = replace_method(v, "    private fun drawUpperFx(c: Canvas, r: RectF, s: SamplerAppController.UpperState)", new_upper_fx)

# Shared COMBI effects: all six numeric controls become live sliders.
new_fx_slot = r'''    private fun drawFxSlot(c: Canvas, r: RectF, slot: Int, fx: SamplerAppController.FxState) {
        panel(c, r, C_SUBPANEL, 10f.dp)
        text(c, "FX$slot", r.left + 16f.dp, r.top + 28f.dp, 17f.dp, C_ACCENT, bold = true)
        val typeR = RectF(r.right - 145f.dp, r.top + 10f.dp, r.right - 15f.dp, r.top + 45f.dp)
        button(c, typeR, effectName(fx.type), false)
        hit(typeR, { _, _ -> val n = (fx.type + 1) % 4; if (sampler.updateCombiFx(slot, fx.copy(type = n))) refreshFx() }, { if (sampler.updateCombiFx(slot, fx.copy(type = 0))) refreshFx() })
        val params = listOf(
            "WET" to fx.wet, "TIME ms" to fx.timeMs, "FEEDBACK" to fx.feedback,
            "RATE Hz" to fx.rateHz, "DEPTH" to fx.depth, "TONE" to fx.tone
        )
        val cols = 3; val rows = 2; val gap = 8f.dp
        val areaTop = r.top + 58f.dp
        val cw = (r.width() - 32f.dp - gap * (cols - 1)) / cols
        val ch = (r.bottom - areaTop - 16f.dp - gap * (rows - 1)) / rows
        params.forEachIndexed { i, pair ->
            val col = i % cols; val row = i / cols
            val rr = RectF(r.left + 16f.dp + col * (cw + gap), areaTop + row * (ch + gap), r.left + 16f.dp + col * (cw + gap) + cw, areaTop + row * (ch + gap) + ch)
            val lim = when (i) { 0 -> 0f to 1f; 1 -> 1f to 2500f; 2 -> 0f to .95f; 3 -> .05f to 8f; else -> 0f to 1f }
            val def = when (i) { 0 -> 0f; 1 -> 320f; 2 -> .25f; 3 -> .8f; 4 -> .35f; else -> .5f }
            val step = when (i) { 1 -> 1f; else -> 0.01f }
            val decimals = if (i == 1) 0 else 2
            text(c, pair.first, rr.centerX(), rr.top + 16f.dp, 9f.dp, C_TEXT_MUTED, alignCenter = true, maxWidth = rr.width() - 8f.dp)
            val bar = RectF(rr.left + 8f.dp, rr.centerY() - 6f.dp, rr.right - 8f.dp, rr.centerY() + 6f.dp)
            drawValueSlider(c, bar, pair.second, lim.first, lim.second)
            registerSmoothSlider(bar, lim.first, lim.second, step) { nv ->
                val nf = when (i) { 0 -> fx.copy(wet = nv); 1 -> fx.copy(timeMs = nv); 2 -> fx.copy(feedback = nv); 3 -> fx.copy(rateHz = nv); 4 -> fx.copy(depth = nv); else -> fx.copy(tone = nv) }
                if (sampler.updateCombiFx(slot, nf)) refreshFx()
            }
            val valueR = RectF(rr.left, rr.bottom - 34f.dp, rr.right, rr.bottom)
            val shown = if (decimals == 0) pair.second.toInt().toString() else "%.2f".format(pair.second)
            text(c, shown, rr.centerX(), rr.bottom - 7f.dp, 10f.dp, C_TEXT, alignCenter = true, bold = true, maxWidth = rr.width() - 6f.dp)
            hit(valueR, { _, _ -> showNumber("FX$slot ${pair.first}", pair.second, lim.first, lim.second, decimals) { nv ->
                val nf = when (i) { 0 -> fx.copy(wet = nv); 1 -> fx.copy(timeMs = nv); 2 -> fx.copy(feedback = nv); 3 -> fx.copy(rateHz = nv); 4 -> fx.copy(depth = nv); else -> fx.copy(tone = nv) }
                if (sampler.updateCombiFx(slot, nf)) refreshFx()
            } }, { val nf = when (i) { 0 -> fx.copy(wet = def); 1 -> fx.copy(timeMs = def); 2 -> fx.copy(feedback = def); 3 -> fx.copy(rateHz = def); 4 -> fx.copy(depth = def); else -> fx.copy(tone = def) }; if (sampler.updateCombiFx(slot, nf)) refreshFx() })
        }
    }
'''
v = replace_method(v, "    private fun drawFxSlot(c: Canvas, r: RectF, slot: Int, fx: SamplerAppController.FxState)", new_fx_slot)

# -----------------------------------------------------------------------------
# Every numeric editor now has a smooth SeekBar plus exact text entry and +/-.
# This also upgrades SETTINGS values that are cards rather than on-canvas bars.
# -----------------------------------------------------------------------------
new_show_number = r'''    private fun showNumber(title:String,current:Float,minV:Float,maxV:Float,decimals:Int,done:(Float)->Unit){
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        fun formatEditorValue(value: Float): String = if (decimals == 0) kotlin.math.round(value).toInt().toString() else "% .${decimals}f".format(value).trim()
        val scale = when { decimals <= 0 -> 1; decimals == 1 -> 10; else -> 100 }
        val sliderMax = max(1, kotlin.math.round((maxV - minV) * scale).toInt())
        body.addView(editorLabel("VALUE"))
        val edit = editorField(formatEditorValue(current.coerceIn(minV, maxV)))
        body.addView(edit)
        body.addView(editorLabel("RANGE   ${formatEditorValue(minV)}  …  ${formatEditorValue(maxV)}"))
        var syncing = false
        val seek = SeekBar(context).apply {
            max = sliderMax
            progress = kotlin.math.round((current.coerceIn(minV, maxV) - minV) * scale).toInt().coerceIn(0, sliderMax)
        }
        fun setEditorValue(value: Float) {
            val clamped = value.coerceIn(minV, maxV)
            syncing = true
            val newText = formatEditorValue(clamped)
            edit.setText(newText)
            edit.setSelection(newText.length)
            seek.progress = kotlin.math.round((clamped - minV) * scale).toInt().coerceIn(0, sliderMax)
            syncing = false
        }
        seek.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                if (!fromUser || syncing) return
                setEditorValue(minV + progress.toFloat() / scale.toFloat())
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
        })
        body.addView(seek)
        val fine = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; setPadding(0, uiPx(8f), 0, 0) }
        val step = 1f / scale.toFloat()
        fun currentTyped(): Float = edit.text.toString().replace(',', '.').toFloatOrNull()?.coerceIn(minV, maxV) ?: current.coerceIn(minV, maxV)
        val minus = Button(context).apply { text = "−"; textSize = 20f; setOnClickListener { setEditorValue(currentTyped() - step) } }
        val plus = Button(context).apply { text = "+"; textSize = 20f; setOnClickListener { setEditorValue(currentTyped() + step) } }
        fine.addView(minus, LinearLayout.LayoutParams(0, uiPx(48f), 1f))
        fine.addView(plus, LinearLayout.LayoutParams(0, uiPx(48f), 1f))
        body.addView(fine)
        showEditorPage(title, "Drag smoothly or type the exact value.", body) {
            val value = edit.text.toString().replace(',', '.').toFloatOrNull()?.coerceIn(minV, maxV) ?: current.coerceIn(minV, maxV)
            done(value)
            invalidate()
        }
    }
'''
v = replace_method(v, "    private fun showNumber(", new_show_number)

# -----------------------------------------------------------------------------
# Typography: fit long dynamic names inside their own rectangles rather than
# painting over adjacent controls. Existing calls remain source-compatible.
# -----------------------------------------------------------------------------
text_start = v.find("    private fun text(c:Canvas,s:String,x:Float,y:Float,size:Float,color:Int,alignCenter:Boolean=false,bold:Boolean=false)")
if text_start < 0:
    text_start = v.find("    private fun text(c: Canvas, s: String")
if text_start < 0:
    raise SystemExit("text helper missing")
line_end = v.find('\n', text_start)
if line_end < 0:
    raise SystemExit("text helper line end missing")
if "maxWidth:Float" not in v[text_start:line_end]:
    replacement = r'''    private fun text(c:Canvas,s:String,x:Float,y:Float,size:Float,color:Int,alignCenter:Boolean=false,bold:Boolean=false,maxWidth:Float=Float.POSITIVE_INFINITY){
        paint.color=color
        paint.textSize=size
        paint.typeface=if(bold)Typeface.create("sans-serif-medium",Typeface.NORMAL) else Typeface.create("sans-serif",Typeface.NORMAL)
        paint.textAlign=if(alignCenter)Paint.Align.CENTER else Paint.Align.LEFT
        var shown=s
        if(maxWidth.isFinite() && maxWidth>1f){
            val measured=paint.measureText(shown)
            if(measured>maxWidth){
                val minSize=max(8f.dp,size*0.72f)
                paint.textSize=max(minSize,size*(maxWidth/measured))
                if(paint.measureText(shown)>maxWidth){
                    val ell="…"
                    val room=max(0f,maxWidth-paint.measureText(ell))
                    val count=paint.breakText(shown,true,room,null)
                    shown=if(count<=0)ell else shown.take(count.coerceAtMost(shown.length))+ell
                }
            }
        }
        c.drawText(shown,x,y,paint)
        paint.textAlign=Paint.Align.LEFT
    }'''
    v = v[:text_start] + replacement + v[line_end:]

# Fit all button labels automatically.
for old, new in [
    ('private fun button(c:Canvas,r:RectF,label:String,active:Boolean,accent:Int=C_ACCENT){silvKey(c,r,active,accent==C_PANIC);text(c,label,r.centerX(),r.centerY()+5f.dp,12f.dp,if(active)C_TEXT else C_TEXT_DIM,alignCenter=true,bold=active)}',
     'private fun button(c:Canvas,r:RectF,label:String,active:Boolean,accent:Int=C_ACCENT){silvKey(c,r,active,accent==C_PANIC);text(c,label,r.centerX(),r.centerY()+5f.dp,12f.dp,if(active)C_TEXT else C_TEXT_DIM,alignCenter=true,bold=active,maxWidth=r.width()-14f.dp)}'),
    ('private fun tinyToggle(c:Canvas,r:RectF,label:String,on:Boolean){panel(c,r,if(on)C_BUTTON_ACTIVE else C_BUTTON,6f.dp,if(on)C_ACCENT else C_BORDER);text(c,label,r.centerX(),r.centerY()+4f.dp,10f.dp,if(on)C_TEXT else C_TEXT_MUTED,alignCenter=true,bold=on)}',
     'private fun tinyToggle(c:Canvas,r:RectF,label:String,on:Boolean){panel(c,r,if(on)C_BUTTON_ACTIVE else C_BUTTON,6f.dp,if(on)C_ACCENT else C_BORDER);text(c,label,r.centerX(),r.centerY()+4f.dp,10f.dp,if(on)C_TEXT else C_TEXT_MUTED,alignCenter=true,bold=on,maxWidth=r.width()-8f.dp)}'),
]:
    if old in v:
        v = v.replace(old, new, 1)

# High-risk dynamic names get explicit bounds. These are conservative visual
# changes only; navigation and page structure are not removed.
v = v.replace('text(c, combiName, r.left + pad, r.top + titleH * 0.70f, 31f.dp, C_TEXT, bold = true)',
              'text(c, combiName, r.left + pad, r.top + titleH * 0.70f, 31f.dp, C_TEXT, bold = true, maxWidth = r.width() - 330f.dp)')
v = v.replace('text(c, currentName, left+95f*sx, nowBottom-17f*sy, 42f*ss, C_TEXT)',
              'text(c, currentName, left+95f*sx, nowBottom-17f*sy, 42f*ss, C_TEXT, maxWidth = right-left-380f*sx)')
v = v.replace('text(c, item?.name ?: "—", rr.left+98f*sx, rr.centerY()-5f*sy, 22f*ss,',
              'text(c, item?.name ?: "—", rr.left+98f*sx, rr.centerY()-5f*sy, 22f*ss,')
# Multi-line LIST item call: add bound to the color line if exact source is present.
v = v.replace('if (selected) C_TEXT else if (item != null) C_LABEL else C_FAINT)\n            if (item != null) {',
              'if (selected) C_TEXT else if (item != null) C_LABEL else C_FAINT, maxWidth = rr.width()-118f*sx)\n            if (item != null) {', 1)
# Settings labels/values should never escape their cards.
v = v.replace('text(c,item.first,rr.centerX(),rr.top+23f.dp,9f.dp,C_TEXT_MUTED,alignCenter=true)',
              'text(c,item.first,rr.centerX(),rr.top+23f.dp,9f.dp,C_TEXT_MUTED,alignCenter=true,maxWidth=rr.width()-16f.dp)')
v = v.replace('text(c,item.second,rr.centerX(),rr.centerY()+12f.dp,18f.dp,C_TEXT,alignCenter=true)',
              'text(c,item.second,rr.centerX(),rr.centerY()+12f.dp,18f.dp,C_TEXT,alignCenter=true,maxWidth=rr.width()-16f.dp)')

view.write_text(v, encoding="utf-8")

# -----------------------------------------------------------------------------
# Technical guard: this polish may change only the View. Re-audit the proven
# Direct ALSA / MIDI contracts and explicitly reject unsafe protection changes.
# -----------------------------------------------------------------------------
b = bridge.read_text(encoding="utf-8")
k = root_mode.read_text(encoding="utf-8")
r = runtime.read_text(encoding="utf-8")
m = midi.read_text(encoding="utf-8")
e = engine.read_text(encoding="utf-8")

for token in [
    'constexpr unsigned kRate = 48000U;',
    'constexpr unsigned kPeriods = 3U;',
    'kPreferredMinimumPeriodFrames = 96U;',
    'config.bufferFrames - config.periodFrames',
    'sw.xfer_align = 1U;',
    'SNDRV_PCM_IOCTL_PREPARE',
    'configureRealtimeThread("SilvAudioRT"',
    'configureRealtimeThread("SilvMidiRT"',
]:
    if token not in b:
        raise SystemExit(f"realtime audio/MIDI contract missing after UI polish: {token}")
if '#include <aaudio/AAudio.h>' in b:
    raise SystemExit('AAudio must not return to the Direct ALSA sampler')

for token in [
    'refreshAudioPermissions', 'refreshMidiPermissions', 'magiskpolicy',
    '0x0000-0xffff', 'ADDA_DL_CH1 DL1_CH1', 'ADDA_DL_CH2 DL1_CH2',
    'DAC In Mux', 'HPL Mux', 'HPR Mux', 'Headset Volume" 12 12',
    'promote SilvAudioRT 80 85', 'promote SilvMidiRT 40 75',
]:
    if token not in k:
        raise SystemExit(f"root realtime/protection contract missing after UI polish: {token}")

for forbidden in [
    'setenforce 0', 'setenforce permissive', '/sys/fs/selinux/enforce',
    '/sys/class/thermal', 'thermal_zone', 'speaker_protect', 'Speaker Protect',
    'SmartPA', 'smartpa', 'chmod 0777', 'chmod 777',
]:
    if forbidden in k:
        raise SystemExit(f"unsafe protection override detected: {forbidden}")

if 'SilvetonRootRuntime.enter()' not in r or 'refreshAudioPermissions()' not in r:
    raise SystemExit('audio lifecycle lost root/permission refresh')
if 'refreshMidiPermissions' not in m:
    raise SystemExit('MIDI hotplug permission refresh missing')
if 'nativeMidi' not in m:
    raise SystemExit('MIDI -> native sampler path missing')
if 'postPanic' not in e or 'postNoteOn' not in e or 'postNoteOff' not in e:
    raise SystemExit('sampler MIDI note/panic safety path missing')

final_v = view.read_text(encoding="utf-8")
for token in [
    'private data class SmoothSliderHit(', 'private data class SmoothRangeHit(',
    'handleSmoothControlTouch(event)', 'registerSmoothSlider(', 'registerSmoothRange(',
    'private fun drawValueSlider(', 'import android.widget.SeekBar',
    'SeekBar.OnSeekBarChangeListener', 'maxWidth:Float=Float.POSITIVE_INFINITY',
    'Typeface.create("sans-serif-medium"', 'drawValueSlider(c, bar',
]:
    if token not in final_v:
        raise SystemExit(f"professional UI contract missing: {token}")

# The fake knob surfaces in the edited TUNE/TONE/FX functions must be gone.
for sig in [
    '    private fun drawTune(c: Canvas, r: RectF, s: SamplerAppController.UpperState)',
    '    private fun drawToneSection(c: Canvas, r: RectF, title: String, values: List<Pair<String, Float>>, commit: (Int, Float) -> Unit)',
    '    private fun drawFxSlot(c: Canvas, r: RectF, slot: Int, fx: SamplerAppController.FxState)',
]:
    a, z = method_bounds(final_v, sig)
    if 'drawKnob(' in final_v[a:z]:
        raise SystemExit(f"decorative fake knob remains in {sig.strip()}")

print('PASS: professional smooth controls + exact entry + bounded typography; Direct ALSA/MIDI protections preserved')
