from pathlib import Path
import sys

root = Path(sys.argv[1])
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
if not view.is_file():
    raise SystemExit(f'missing view: {view}')

s = view.read_text(encoding='utf-8')


def replace_method(text: str, start_sig: str, next_sig: str, body: str) -> str:
    a = text.find(start_sig)
    if a < 0:
        raise SystemExit(f'missing method: {start_sig}')
    b = text.find(next_sig, a)
    if b < 0:
        raise SystemExit(f'missing next method after: {start_sig}')
    return text[:a] + body.rstrip() + '\n\n' + text[b:]

# Final live header contract. MIDI lamp flashes only on NOTE ON; AUDIO lamp follows
# real voice/meter activity. The status text still reports connection/driver state.
header = r'''    private fun drawHeader(c: Canvas, w: Float, h: Float) {
        val sx = w / DESIGN_W
        val sy = h / DESIGN_H
        val ss = min(sx, sy)
        val hh = 76f * sy
        paint.shader = LinearGradient(0f, 0f, 0f, hh, C_BG_TOP, C_BG, Shader.TileMode.CLAMP)
        c.drawRect(0f, 0f, w, hh, paint)
        paint.shader = null
        line(c, 16f*sx, hh-1f*sy, w-16f*sx, hh-1f*sy, Color.argb(28,255,255,255), 1f*ss)

        val gear = RectF(16f*sx,12f*sy,84f*sx,64f*sy)
        silvKey(c,gear,page==Page.SETTINGS,false)
        drawGear(c,gear.centerX(),gear.centerY(),12f*ss)
        hit(gear,{_,_-> page=Page.SETTINGS; refreshGlobal()})

        val panic = RectF(96f*sx,12f*sy,244f*sx,64f*sy)
        silvKey(c,panic,false,true)
        silvLed(c,panic.left+20f*sx,panic.centerY(),true,true,5.5f*ss)
        text(c,"PANIC",panic.centerX()+8f*sx,panic.centerY()+6f*sy,17f*ss,C_TEXT,alignCenter=true,bold=true)
        hit(panic,{_,_->sampler.panic()})

        val test = RectF(254f*sx,12f*sy,374f*sx,64f*sy)
        val testReady = sampler.testInstrumentEnabled()
        silvKey(c,test,testReady,false,enabled=testReady)
        silvLed(c,test.left+18f*sx,test.centerY(),testReady,false,4.5f*ss)
        text(c,"PIANO",test.centerX()+7f*sx,test.centerY()+6f*sy,14f*ss,if(testReady)C_TEXT else C_TEXT_MUTED,alignCenter=true,bold=testReady)
        if(testReady) hit(test,{_,_-> if(sampler.testEpNoteOn(60,110)) postDelayed({sampler.testEpNoteOff(60)},650L)})

        drawBrand(c,w*0.5f,38f*sy,sx,sy)

        val a = frameAudioStatus
        val m = frameMidiStatus
        val now = android.os.SystemClock.elapsedRealtime()
        val midiRx = now < midiFlashUntilMs
        val keyFlash = midiRx && (m.status and 0xF0) == 0x90 && m.data2 > 0
        val midiConnected = max(midiConnections,m.deviceCount) > 0
        val midiValue = when {
            keyFlash -> "${midiNoteName(m.data1)}"
            midiRx -> "RX"
            midiConnected -> "ON"
            m.lastError != 0 -> "E${-m.lastError}"
            else -> "OFF"
        }

        val running = sampler.audioRunning()
        val lastAudioError = sampler.audioLastError()
        val audioOk = running && a.sampleRate == 48000 && a.framesPerBurst == 96 && a.bufferSizeFrames == 192
        val audioSignal = a.activeVoices > 0 || frameUpperMeters.any { it.first > 0.0001f || it.second > 0.0001f }
        val audioValue = when {
            audioOk -> "48k 96x2"
            running -> "${a.sampleRate/1000}k ${a.framesPerBurst}"
            lastAudioError != 0 -> "OFF E${-lastAudioError}"
            else -> "OFF"
        }

        val chipY=16f*sy; val chipH=44f*sy; val gap=8f*sx; var right=w-16f*sx
        val midiR=RectF(right-124f*sx,chipY,right,chipY+chipH)
        silvStatus(c,midiR,"MIDI",midiValue,keyFlash,sx,sy); right=midiR.left-gap
        val xrunR=RectF(right-108f*sx,chipY,right,chipY+chipH)
        silvStatus(c,xrunR,"XRUN",a.xrunCount.toString(),audioOk&&a.xrunCount==0,sx,sy); right=xrunR.left-gap
        val audioR=RectF(right-164f*sx,chipY,right,chipY+chipH)
        silvStatus(c,audioR,"AUDIO",audioValue,audioSignal,sx,sy)
    }'''
s = replace_method(s, '    private fun drawHeader(', '    private fun drawRightNav(', header)

# Make the top MIDI and AUDIO indicators actual lamps. MIDI flashes on NOTE ON;
# AUDIO lights only while the DSP reports active voices or real output meter activity.
status = r'''    private fun silvStatus(c: Canvas, r: RectF, label: String, value: String, on: Boolean, sx: Float, sy: Float) {
        val ss = min(sx, sy)
        paint.style = Paint.Style.FILL
        paint.shader = LinearGradient(0f, r.top, 0f, r.bottom, Color.rgb(27,33,42), Color.rgb(18,23,30), Shader.TileMode.CLAMP)
        c.drawRoundRect(r, r.height()*0.5f, r.height()*0.5f, paint)
        paint.shader = null
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1f*ss
        paint.color = Color.argb(38,255,255,255)
        c.drawRoundRect(r, r.height()*0.5f, r.height()*0.5f, paint)
        paint.style = Paint.Style.FILL

        if (label == "MIDI" || label == "AUDIO") {
            silvLed(c, r.left+18f*sx, r.centerY(), on, false, if (on) 6.5f*ss else 5.5f*ss)
            text(c, label, r.left+34f*sx, r.centerY()-3f*sy, 10f*ss, C_TEXT_MUTED, bold=true)
            text(c, value, r.left+34f*sx, r.centerY()+14f*sy, 15f*ss, if(on) C_TEXT else C_LABEL, bold=on)
        } else {
            text(c, label, r.left+13f*sx, r.centerY()-3f*sy, 10f*ss, C_TEXT_MUTED, bold=true)
            text(c, value, r.left+13f*sx, r.centerY()+14f*sy, 17f*ss, C_TEXT, bold=true)
        }
    }'''
s = replace_method(s, '    private fun silvStatus(', '    private fun lighten(', status)

# The forced built-in test instrument is a real DSP sound and is already driven by
# external raw MIDI. When the user's set list is empty, expose it as slot 001 rather
# than showing an empty page. It is displayed as selected because it is the active
# fallback instrument in this hardware-validation build.
s = s.replace(
    '        val currentName = activeItem?.name ?: if (sampler.testInstrumentEnabled()) "TEST EP" else "Силветон"\n',
    '        val currentName = activeItem?.name ?: if (sampler.testInstrumentEnabled()) "STAGE PIANO" else "Силветон"\n',
    1,
)

start_anchor = '        val start = listPage*10\n\n        for (j in 0 until 10) {'
if start_anchor not in s:
    raise SystemExit('set-list grid anchor missing')
s = s.replace(
    start_anchor,
    '        val start = listPage*10\n'
    '        val builtInPiano = entries.isEmpty() && sampler.testInstrumentEnabled()\n\n'
    '        for (j in 0 until 10) {',
    1,
)

item_anchor = '''            val item = entries.getOrNull(idx)
            val selected = idx == active
            silvKey(c, rr, selected, false, enabled=item != null)
'''
if item_anchor not in s:
    raise SystemExit('set-list item anchor missing')
item_replacement = '''            val item = entries.getOrNull(idx)
            if (builtInPiano && idx == 0) {
                silvKey(c, rr, true, false, enabled=true)
                silvLed(c, rr.left+18f*sx, rr.centerY(), true, false, 5.5f*ss)
                text(c, "001", rr.left+42f*sx, rr.centerY()-8f*sy, 17f*ss, C_ACCENT, bold=true)
                text(c, "STAGE PIANO", rr.left+98f*sx, rr.centerY()-5f*sy, 22f*ss, C_TEXT, bold=true)
                text(c, "BUILT-IN  •  RAW MIDI", rr.left+98f*sx, rr.centerY()+19f*sy, 12f*ss, C_TEXT_MUTED)
                continue
            }
            val selected = idx == active
            silvKey(c, rr, selected, false, enabled=item != null)
'''
s = s.replace(item_anchor, item_replacement, 1)

required = [
    'val keyFlash = midiRx && (m.status and 0xF0) == 0x90 && m.data2 > 0',
    'val audioSignal = a.activeVoices > 0 || frameUpperMeters.any',
    'a.sampleRate == 48000',
    '"48k 96x2"',
    'silvStatus(c,midiR,"MIDI",midiValue,keyFlash',
    'silvStatus(c,audioR,"AUDIO",audioValue,audioSignal',
    '"STAGE PIANO"',
    '"BUILT-IN  •  RAW MIDI"',
]
for needle in required:
    if needle not in s:
        raise SystemExit(f'live performance contract missing: {needle}')

view.write_text(s, encoding='utf-8')
print('PASS: live MIDI key lamp + audio signal lamp + built-in Stage Piano set-list slot installed')
