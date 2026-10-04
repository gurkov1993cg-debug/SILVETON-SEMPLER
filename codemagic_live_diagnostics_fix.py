from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root/'android/app/src/main/cpp/SamplerAudioBridge.cpp'
native = root/'android/app/src/main/java/com/silveton/sampler/NativeSamplerEngine.kt'
controller = root/'android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt'
view = root/'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
for p in (cpp, native, controller, view):
    if not p.is_file():
        raise SystemExit(f'missing {p}')

# ---------- Native C++ diagnostics ----------
s = cpp.read_text(encoding='utf-8')

# Ensure MIDI activity serial exists even if an earlier patch changed its anchor.
if 'std::atomic<std::uint64_t> midiActivitySerial_' not in s:
    anchor = '    BuiltinTestInstrument testInstrument_{};\n'
    if anchor not in s:
        raise SystemExit('BuiltinTestInstrument member anchor missing')
    s = s.replace(anchor, anchor + '    std::atomic<std::uint64_t> midiActivitySerial_{0U};\n', 1)

# Add last-message atomics once.
member_anchor = '    std::atomic<std::uint64_t> midiActivitySerial_{0U};\n'
member_add = (
    member_anchor +
    '    std::atomic<int> midiLastStatus_{0};\n'
    '    std::atomic<int> midiLastData1_{0};\n'
    '    std::atomic<int> midiLastData2_{0};\n'
)
if 'std::atomic<int> midiLastStatus_' not in s:
    if member_anchor not in s:
        raise SystemExit('MIDI serial member anchor missing')
    s = s.replace(member_anchor, member_add, 1)

# Locate the midi() body structurally and insert diagnostics after channel decode.
if 'midiLastStatus_.store' not in s:
    midi_start = s.find('    bool midi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept {')
    if midi_start < 0:
        raise SystemExit('midi() method missing')
    switch_pos = s.find('        switch (kind) {', midi_start)
    if switch_pos < 0:
        raise SystemExit('midi() switch missing')
    body = s[midi_start:switch_pos]
    ch_line = '        const std::uint8_t ch = static_cast<std::uint8_t>(status & 0x0FU);\n'
    rel = body.find(ch_line)
    if rel < 0:
        raise SystemExit('midi() channel decode missing')
    insert_pos = midi_start + rel + len(ch_line)
    diag = (
        '        midiLastStatus_.store(static_cast<int>(status), std::memory_order_relaxed);\n'
        '        midiLastData1_.store(static_cast<int>(data1), std::memory_order_relaxed);\n'
        '        midiLastData2_.store(static_cast<int>(data2), std::memory_order_relaxed);\n'
    )
    # If the serial increment is already elsewhere in the midi pre-switch body, keep it.
    if 'midiActivitySerial_.fetch_add' not in body:
        diag += '        midiActivitySerial_.fetch_add(1U, std::memory_order_release);\n'
    s = s[:insert_pos] + diag + s[insert_pos:]

# Upgrade any existing serial increment to release ordering for UI observation.
s = s.replace('midiActivitySerial_.fetch_add(1U, std::memory_order_relaxed);',
              'midiActivitySerial_.fetch_add(1U, std::memory_order_release);')

# Public getters/test trigger. Insert independently so previous partial patches are accepted.
getter_anchor = '    int midiLastError() const noexcept { return midiLastError_.load(std::memory_order_relaxed); }\n'
if getter_anchor not in s:
    raise SystemExit('midiLastError getter missing')
getters = ''
if 'midiActivitySerial() const noexcept' not in s:
    getters += '    std::uint64_t midiActivitySerial() const noexcept { return midiActivitySerial_.load(std::memory_order_acquire); }\n'
if 'midiLastStatus() const noexcept' not in s:
    getters += '    int midiLastStatus() const noexcept { return midiLastStatus_.load(std::memory_order_relaxed); }\n'
if 'midiLastData1() const noexcept' not in s:
    getters += '    int midiLastData1() const noexcept { return midiLastData1_.load(std::memory_order_relaxed); }\n'
if 'midiLastData2() const noexcept' not in s:
    getters += '    int midiLastData2() const noexcept { return midiLastData2_.load(std::memory_order_relaxed); }\n'
if 'testInstrumentEnabled() const noexcept' not in s:
    getters += '    bool testInstrumentEnabled() const noexcept { return testInstrument_.enabled(); }\n'
if 'testNoteOn(std::uint8_t note' not in s:
    getters += (
        '    bool testNoteOn(std::uint8_t note, std::uint8_t velocity) noexcept {\n'
        '        if (!testInstrument_.enabled() || velocity == 0U) return false;\n'
        '        testInstrument_.noteOn(note, velocity);\n'
        '        return true;\n'
        '    }\n'
    )
if 'testNoteOff(std::uint8_t note' not in s:
    getters += '    void testNoteOff(std::uint8_t note) noexcept { testInstrument_.noteOff(note); }\n'
if getters:
    s = s.replace(getter_anchor, getter_anchor + getters, 1)

# JNI diagnostics. Add each family once after nativeMidiLastError.
jni_anchor = 'extern "C" JNIEXPORT jint JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiLastError(JNIEnv*,jobject){return sampler().midiLastError();}\n'
if jni_anchor not in s:
    raise SystemExit('JNI midiLastError anchor missing')
jni = ''
if 'Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiActivitySerial' not in s:
    jni += 'extern "C" JNIEXPORT jlong JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiActivitySerial(JNIEnv*,jobject){return static_cast<jlong>(sampler().midiActivitySerial());}\n'
if 'Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiLastMessage' not in s:
    jni += 'extern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiLastMessage(JNIEnv* env,jobject){const std::array<jint,3>v{{sampler().midiLastStatus(),sampler().midiLastData1(),sampler().midiLastData2()}};jintArray a=env->NewIntArray(3);if(a!=nullptr)env->SetIntArrayRegion(a,0,3,v.data());return a;}\n'
if 'Java_com_silveton_sampler_NativeSamplerEngine_nativeAudioStarted' not in s:
    jni += 'extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAudioStarted(JNIEnv*,jobject){return sampler().started()?JNI_TRUE:JNI_FALSE;}\n'
if 'Java_com_silveton_sampler_NativeSamplerEngine_nativeTestInstrumentEnabled' not in s:
    jni += 'extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeTestInstrumentEnabled(JNIEnv*,jobject){return sampler().testInstrumentEnabled()?JNI_TRUE:JNI_FALSE;}\n'
if 'Java_com_silveton_sampler_NativeSamplerEngine_nativeTestNoteOn' not in s:
    jni += 'extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeTestNoteOn(JNIEnv*,jobject,jint note,jint velocity){return sampler().testNoteOn(static_cast<std::uint8_t>(std::clamp(note,0,127)),static_cast<std::uint8_t>(std::clamp(velocity,0,127)))?JNI_TRUE:JNI_FALSE;}\n'
if 'Java_com_silveton_sampler_NativeSamplerEngine_nativeTestNoteOff' not in s:
    jni += 'extern "C" JNIEXPORT void JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeTestNoteOff(JNIEnv*,jobject,jint note){sampler().testNoteOff(static_cast<std::uint8_t>(std::clamp(note,0,127)));}\n'
if jni:
    s = s.replace(jni_anchor, jni_anchor + jni, 1)
cpp.write_text(s, encoding='utf-8')

# ---------- Kotlin native declarations ----------
k = native.read_text(encoding='utf-8')
anchor = '    external fun nativeMidiLastError(): Int\n'
if anchor not in k:
    raise SystemExit('NativeSamplerEngine MIDI anchor missing')
add = ''
for decl in [
    '    external fun nativeMidiActivitySerial(): Long\n',
    '    external fun nativeMidiLastMessage(): IntArray\n',
    '    external fun nativeAudioStarted(): Boolean\n',
    '    external fun nativeTestInstrumentEnabled(): Boolean\n',
    '    external fun nativeTestNoteOn(note: Int, velocity: Int): Boolean\n',
    '    external fun nativeTestNoteOff(note: Int)\n',
]:
    if decl.strip() not in k:
        add += decl
if add:
    k = k.replace(anchor, anchor + add, 1)
native.write_text(k, encoding='utf-8')

# ---------- Controller wrappers ----------
k = controller.read_text(encoding='utf-8')
if 'data class MidiStatus(' not in k:
    class_anchor = 'class SamplerAppController('
    class_pos = k.find(class_anchor)
    if class_pos < 0:
        raise SystemExit('SamplerAppController class missing')
    brace = k.find('{', class_pos)
    if brace < 0:
        raise SystemExit('SamplerAppController opening brace missing')
    block = '''\n    data class MidiStatus(\n        val deviceCount: Int,\n        val lastError: Int,\n        val activitySerial: Long,\n        val status: Int,\n        val data1: Int,\n        val data2: Int\n    )\n'''
    k = k[:brace+1] + block + k[brace+1:]

# Add lightweight wrapper methods before panic(), avoiding AudioStatus schema churn.
if 'fun midiStatus(): MidiStatus' not in k:
    anchor = '    fun panic()'
    pos = k.find(anchor)
    if pos < 0:
        raise SystemExit('controller panic anchor missing')
    methods = '''    fun midiStatus(): MidiStatus {\n        val msg = engine.nativeMidiLastMessage()\n        return MidiStatus(\n            deviceCount = engine.nativeMidiDeviceCount().coerceAtLeast(0),\n            lastError = engine.nativeMidiLastError(),\n            activitySerial = engine.nativeMidiActivitySerial(),\n            status = msg.getOrElse(0) { 0 },\n            data1 = msg.getOrElse(1) { 0 },\n            data2 = msg.getOrElse(2) { 0 }\n        )\n    }\n\n    fun audioRunning(): Boolean = !closed.get() && engine.nativeAudioStarted()\n    fun audioLastError(): Int = engine.nativeLastAudioError()\n    fun testInstrumentEnabled(): Boolean = !closed.get() && engine.nativeTestInstrumentEnabled()\n    fun testEpNoteOn(note: Int = 60, velocity: Int = 110): Boolean =\n        !closed.get() && engine.nativeTestNoteOn(note.coerceIn(0,127), velocity.coerceIn(1,127))\n    fun testEpNoteOff(note: Int = 60) { if (!closed.get()) engine.nativeTestNoteOff(note.coerceIn(0,127)) }\n\n'''
    k = k[:pos] + methods + k[pos:]
controller.write_text(k, encoding='utf-8')

# ---------- GUI live status ----------
k = view.read_text(encoding='utf-8')
field_anchor = '    private var frameUpperMeters: List<Pair<Float, Float>> = List(7) { 0f to 0f }\n'
if 'private var frameMidiStatus:' not in k:
    if field_anchor not in k:
        raise SystemExit('view frame meter anchor missing')
    fields = (
        field_anchor +
        '    private var frameMidiStatus: SamplerAppController.MidiStatus = sampler.midiStatus()\n'
        '    private var lastMidiSerial = frameMidiStatus.activitySerial\n'
        '    private var midiFlashUntilMs = 0L\n'
    )
    k = k.replace(field_anchor, fields, 1)

if 'midiFlashUntilMs = android.os.SystemClock.elapsedRealtime() + 220L' not in k:
    draw_anchor = '        frameUpperMeters = sampler.upperMeters()\n'
    if draw_anchor not in k:
        raise SystemExit('view onDraw meter anchor missing')
    draw_add = (
        draw_anchor +
        '        frameMidiStatus = sampler.midiStatus()\n'
        '        if (frameMidiStatus.activitySerial != lastMidiSerial) {\n'
        '            lastMidiSerial = frameMidiStatus.activitySerial\n'
        '            midiFlashUntilMs = android.os.SystemClock.elapsedRealtime() + 220L\n'
        '        }\n'
    )
    k = k.replace(draw_anchor, draw_add, 1)

def replace_method(text: str, start_sig: str, next_sig: str, body: str) -> str:
    a = text.find(start_sig)
    if a < 0:
        raise SystemExit(f'missing method {start_sig}')
    b = text.find(next_sig, a)
    if b < 0:
        raise SystemExit(f'missing next method after {start_sig}')
    return text[:a] + body.rstrip() + '\n\n' + text[b:]

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
        text(c,"TEST EP",test.centerX()+7f*sx,test.centerY()+6f*sy,14f*ss,if(testReady)C_TEXT else C_TEXT_MUTED,alignCenter=true,bold=testReady)
        if(testReady) hit(test,{_,_-> if(sampler.testEpNoteOn(60,110)) postDelayed({sampler.testEpNoteOff(60)},650L)})

        drawBrand(c,w*0.5f,38f*sy,sx,sy)

        val a = frameAudioStatus
        val m = frameMidiStatus
        val now = android.os.SystemClock.elapsedRealtime()
        val midiRx = now < midiFlashUntilMs
        val midiConnected = max(midiConnections,m.deviceCount) > 0
        val midiValue = when {
            midiRx && (m.status and 0xF0) == 0x90 && m.data2 > 0 -> "RX ${midiNoteName(m.data1)}"
            midiRx -> "RX"
            midiConnected -> "ON"
            m.lastError != 0 -> "E${-m.lastError}"
            else -> "OFF"
        }
        val running = sampler.audioRunning()
        val lastAudioError = sampler.audioLastError()
        val audioOk = running && a.sampleRate == 96000 && a.framesPerBurst == 96 && a.bufferSizeFrames == 192
        val audioValue = when {
            audioOk -> "96k 96x2"
            running -> "${a.sampleRate/1000}k ${a.framesPerBurst}"
            lastAudioError != 0 -> "OFF E${-lastAudioError}"
            else -> "OFF"
        }

        val chipY=16f*sy; val chipH=44f*sy; val gap=8f*sx; var right=w-16f*sx
        val midiR=RectF(right-124f*sx,chipY,right,chipY+chipH)
        silvStatus(c,midiR,"MIDI",midiValue,midiConnected||midiRx,sx,sy); right=midiR.left-gap
        val xrunR=RectF(right-108f*sx,chipY,right,chipY+chipH)
        silvStatus(c,xrunR,"XRUN",a.xrunCount.toString(),audioOk&&a.xrunCount==0,sx,sy); right=xrunR.left-gap
        val audioR=RectF(right-164f*sx,chipY,right,chipY+chipH)
        silvStatus(c,audioR,"AUDIO",audioValue,audioOk,sx,sy)
    }'''
k = replace_method(k, '    private fun drawHeader(', '    private fun drawRightNav(', header)

# Show the fallback sound name when no list entry exists.
k = k.replace(
    '        text(c, activeItem?.name ?: "Силветон", left+95f*sx, nowBottom-17f*sy, 42f*ss, C_TEXT)\n',
    '        val currentName = activeItem?.name ?: if (sampler.testInstrumentEnabled()) "TEST EP" else "Силветон"\n'
    '        text(c, currentName, left+95f*sx, nowBottom-17f*sy, 42f*ss, C_TEXT)\n', 1)

if 'private fun midiNoteName(' not in k:
    anchor = '    private fun colorWithAlpha(color: Int, alpha: Int): Int = Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color))\n'
    if anchor not in k:
        raise SystemExit('view colorWithAlpha anchor missing')
    helper = '''    private fun midiNoteName(note: Int): String {\n        val n = note.coerceIn(0,127)\n        val names = arrayOf("C","C#","D","D#","E","F","F#","G","G#","A","A#","B")\n        return names[n%12] + (n/12 - 1)\n    }\n\n'''
    k = k.replace(anchor, helper + anchor, 1)
view.write_text(k, encoding='utf-8')

# ---------- Contracts ----------
for path, needles in {
    cpp: [
        'midiLastStatus_.store',
        'midiActivitySerial() const noexcept',
        'testNoteOn(std::uint8_t note',
        'nativeMidiActivitySerial',
        'nativeAudioStarted',
        'nativeTestNoteOn',
    ],
    native: [
        'nativeMidiActivitySerial(): Long',
        'nativeAudioStarted(): Boolean',
        'nativeTestNoteOn(note: Int, velocity: Int)',
    ],
    controller: [
        'data class MidiStatus(',
        'fun midiStatus(): MidiStatus',
        'fun audioRunning(): Boolean',
        'fun testEpNoteOn(',
    ],
    view: [
        '"TEST EP"',
        '"AUDIO"',
        '"XRUN"',
        'RX ${midiNoteName(m.data1)}',
        'frameMidiStatus = sampler.midiStatus()',
    ],
}.items():
    text = path.read_text(encoding='utf-8')
    for n in needles:
        if n not in text:
            raise SystemExit(f'live diagnostics contract missing in {path.name}: {n}')

print('PASS: live hardware diagnostics + TEST EP control installed (real AUDIO/XRUN/MIDI state, MIDI RX activity, audible C4 self-test)')
