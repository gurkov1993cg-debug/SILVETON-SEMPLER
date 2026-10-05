#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
view = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"
if not view.is_file():
    raise SystemExit(f"missing sampler view: {view}")


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
        if ch == '{': depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0: return i
    return -1

s = view.read_text(encoding="utf-8")
sig = "    private fun drawUpperRow(c: Canvas, r: RectF, i: Int, s: SamplerAppController.UpperState, meter: Pair<Float, Float>)"
a = s.find(sig)
if a < 0:
    raise SystemExit("drawUpperRow missing")
brace = s.find('{', a)
z = match_brace(s, brace)
if brace < 0 or z < 0:
    raise SystemExit("drawUpperRow boundaries invalid")
z += 1
if z < len(s) and s[z] == '\n': z += 1

replacement = r'''    private fun drawUpperRow(c: Canvas, r: RectF, i: Int, s: SamplerAppController.UpperState, meter: Pair<Float, Float>) {
        val selected = i == selectedUpper
        panel(c, r, if (selected) C_ROW_SELECTED else C_ROW, 9f.dp, if (selected) C_ACCENT_DARK else C_BORDER)
        val x0 = r.left + 13f.dp
        text(c, "UPPER ${i + 1}", x0, r.centerY() + 5f.dp, 12f.dp, if (selected) C_ACCENT else C_TEXT_DIM, bold = selected, maxWidth = r.width() * 0.11f)

        val soundL = r.left + r.width() * 0.13f
        val soundR = r.left + r.width() * 0.36f
        text(c, s.soundName, soundL, r.centerY() + 6f.dp, 13f.dp, if (s.soundName == "EMPTY") C_TEXT_MUTED else C_TEXT, maxWidth = soundR - soundL - 8f.dp)
        hit(RectF(soundL - 4f.dp, r.top, soundR, r.bottom), { _, _ -> selectedUpper = i; showSoundPicker(i) })

        val onR = RectF(r.left + r.width() * 0.365f, r.top + 6f.dp, r.left + r.width() * 0.405f, r.bottom - 6f.dp)
        tinyToggle(c, onR, "ON", s.enabled)
        hit(onR, { _, _ -> if (s.soundName != "EMPTY") commitMain(i, s.copy(enabled = !s.enabled)) })

        val soloR = RectF(r.left + r.width() * 0.410f, r.top + 6f.dp, r.left + r.width() * 0.455f, r.bottom - 6f.dp)
        tinyToggle(c, soloR, "S", s.solo)
        hit(soloR, { _, _ -> commitMain(i, s.copy(solo = !s.solo)) })

        val muteR = RectF(r.left + r.width() * 0.460f, r.top + 6f.dp, r.left + r.width() * 0.505f, r.bottom - 6f.dp)
        tinyToggle(c, muteR, "M", s.mute)
        hit(muteR, { _, _ -> commitMain(i, s.copy(mute = !s.mute)) })

        val volR = RectF(r.left + r.width() * 0.515f, r.top + 5f.dp, r.left + r.width() * 0.655f, r.bottom - 5f.dp)
        val volValue = RectF(volR.left, volR.top, volR.left + 48f.dp, volR.bottom)
        val volBar = RectF(volR.left + 52f.dp, volR.centerY() - 5f.dp, volR.right - 5f.dp, volR.centerY() + 5f.dp)
        text(c, "%.1f".format(s.levelDb), volValue.left, volValue.centerY() + 4f.dp, 10f.dp, C_TEXT, maxWidth = volValue.width() - 3f.dp)
        drawValueSlider(c, volBar, s.levelDb, -60f, 12f)
        registerSmoothSlider(volBar, -60f, 12f, 0.1f) { nv ->
            val current = upperStates[i] ?: s
            commitMain(i, current.copy(levelDb = nv))
        }
        hit(volValue, { _, _ -> showNumber("UPPER ${i + 1} LEVEL dB", s.levelDb, -60f, 12f, 1) { nv ->
            val current = upperStates[i] ?: s
            commitMain(i, current.copy(levelDb = nv))
        } }, { val current = upperStates[i] ?: s; commitMain(i, current.copy(levelDb = 0f)) })

        val panR = RectF(r.left + r.width() * 0.665f, r.top + 5f.dp, r.left + r.width() * 0.755f, r.bottom - 5f.dp)
        val panValue = RectF(panR.left, panR.top, panR.left + 35f.dp, panR.bottom)
        val panBar = RectF(panR.left + 39f.dp, panR.centerY() - 5f.dp, panR.right - 5f.dp, panR.centerY() + 5f.dp)
        text(c, panText(s.pan), panValue.left, panValue.centerY() + 4f.dp, 10f.dp, C_TEXT, maxWidth = panValue.width() - 2f.dp)
        drawValueSlider(c, panBar, s.pan, -1f, 1f)
        registerSmoothSlider(panBar, -1f, 1f, 0.01f) { nv ->
            val current = upperStates[i] ?: s
            commitMain(i, current.copy(pan = nv))
        }
        hit(panValue, { _, _ -> showNumber("UPPER ${i + 1} PAN", s.pan, -1f, 1f, 2) { nv ->
            val current = upperStates[i] ?: s
            commitMain(i, current.copy(pan = nv))
        } }, { val current = upperStates[i] ?: s; commitMain(i, current.copy(pan = 0f)) })

        val rangeR = RectF(r.left + r.width() * 0.765f, r.top, r.left + r.width() * 0.855f, r.bottom)
        text(c, "${noteName(s.keyLow)}  –  ${noteName(s.keyHigh)}", rangeR.centerX(), r.centerY() + 5f.dp, 11f.dp, C_TEXT, alignCenter = true, maxWidth = rangeR.width() - 8f.dp)
        hit(rangeR, { _, _ -> selectedUpper = i; showKeyRange(i, s) })

        val modeR = RectF(r.left + r.width() * 0.86f, r.top + 5f.dp, r.left + r.width() * 0.94f, r.bottom - 5f.dp)
        button(c, modeR, voiceModeName(s.voiceMode), false)
        hit(modeR, { _, _ -> commitMain(i, s.copy(voiceMode = (s.voiceMode + 1) % 3)) }, { commitMain(i, s.copy(voiceMode = 0)) })

        val meterR = RectF(r.left + r.width() * 0.947f, r.top + 10f.dp, r.right - 9f.dp, r.bottom - 10f.dp)
        drawUpperMeter(c, meterR, max(meter.first, meter.second))
        hit(RectF(r.left, r.top, soundL - 6f.dp, r.bottom), { _, _ -> selectedUpper = i })
    }
'''

s = s[:a] + replacement.rstrip() + "\n" + s[z:]
for token in [
    'registerSmoothSlider(volBar, -60f, 12f, 0.1f)',
    'showNumber("UPPER ${i + 1} LEVEL dB"',
    'registerSmoothSlider(panBar, -1f, 1f, 0.01f)',
    'showNumber("UPPER ${i + 1} PAN"',
    'maxWidth = soundR - soundL - 8f.dp',
]:
    if token not in s:
        raise SystemExit(f"UPPER professional control contract missing: {token}")
view.write_text(s, encoding="utf-8")
print("PASS: UPPER level/pan are smooth precise controls with exact entry and bounded sound names")
