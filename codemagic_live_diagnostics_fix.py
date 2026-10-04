from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root/'android/app/src/main/cpp/SamplerAudioBridge.cpp'
native = root/'android/app/src/main/java/com/silveton/sampler/NativeSamplerEngine.kt'
controller = root/'android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt'
view = root/'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
for p in (cpp,native,controller,view):
    if not p.is_file(): raise SystemExit(f'missing {p}')

s = cpp.read_text(encoding='utf-8')
needle = '''        const std::uint8_t kind = static_cast<std::uint8_t>(status & 0xF0U);\n        const std::uint8_t ch = static_cast<std::uint8_t>(status & 0x0FU);\n        midiActivitySerial_.fetch_add(1U, std::memory_order_relaxed);\n'''
repl = '''        const std::uint8_t kind = static_cast<std::uint8_t>(status & 0xF0U);\n        const std::uint8_t ch = static_cast<std::uint8_t>(status & 0x0FU);\n        midiLastStatus_.store(static_cast<int>(status), std::memory_order_relaxed);\n        midiLastData1_.store(static_cast<int>(data1), std::memory_order_relaxed);\n        midiLastData2_.store(static_cast<int>(data2), std::memory_order_relaxed);\n        midiActivitySerial_.fetch_add(1U, std::memory_order_release);\n'''
if needle in s:
    s=s.replace(needle,repl,1)
elif 'midiLastStatus_.store' not in s:
    raise SystemExit('midi activity anchor changed')

getter_anchor = '''    int midiDeviceCount() const noexcept { return midiDeviceCount_.load(std::memory_order_relaxed); }\n    int midiLastError() const noexcept { return midiLastError_.load(std::memory_order_relaxed); }\n'''
getter_new = '''    int midiDeviceCount() const noexcept { return midiDeviceCount_.load(std::memory_order_relaxed); }\n    int midiLastError() const noexcept { return midiLastError_.load(std::memory_order_relaxed); }\n    std::uint64_t midiActivitySerial() const noexcept { return midiActivitySerial_.load(std::memory_order_acquire); }\n    int midiLastStatus() const noexcept { return midiLastStatus_.load(std::memory_order_relaxed); }\n    int midiLastData1() const noexcept { return midiLastData1_.load(std::memory_order_relaxed); }\n    int midiLastData2() const noexcept { return midiLastData2_.load(std::memory_order_relaxed); }\n    bool testInstrumentEnabled() const noexcept { return testInstrument_.enabled(); }\n    bool testNoteOn(std::uint8_t note, std::uint8_t velocity) noexcept {\n        if (!testInstrument_.enabled() || velocity == 0U) return false;\n        testInstrument_.noteOn(note, velocity);\n        return true;\n    }\n    void testNoteOff(std::uint8_t note) noexcept { testInstrument_.noteOff(note); }\n'''
if getter_anchor in s:
    s=s.replace(getter_anchor,getter_new,1)
elif 'midiActivitySerial() const noexcept' not in s:
    raise SystemExit('native getter anchor changed')

member_anchor = '''    BuiltinTestInstrument testInstrument_{};\n    std::atomic<std::uint64_t> midiActivitySerial_{0U};\n'''
member_new = '''    BuiltinTestInstrument testInstrument_{};\n    std::atomic<std::uint64_t> midiActivitySerial_{0U};\n    std::atomic<int> midiLastStatus_{0};\n    std::atomic<int> midiLastData1_{0};\n    std::atomic<int> midiLastData2_{0};\n'''
if member_anchor in s:
    s=s.replace(member_anchor,member_new,1)
elif 'midiLastStatus_{0}' not in s:
    raise SystemExit('native diagnostics member anchor changed')

jni_anchor = '''extern "C" JNIEXPORT jint JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiLastError(JNIEnv*,jobject){return sampler().midiLastError();}\n'''
jni_new = jni_anchor + '''extern "C" JNIEXPORT jlong JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiActivitySerial(JNIEnv*,jobject){return static_cast<jlong>(sampler().midiActivitySerial());}\nextern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiLastMessage(JNIEnv* env,jobject){const std::array<jint,3>v{{sampler().midiLastStatus(),sampler().midiLastData1(),sampler().midiLastData2()}};jintArray a=env->NewIntArray(3);if(a!=nullptr)env->SetIntArrayRegion(a,0,3,v.data());return a;}\nextern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAudioStarted(JNIEnv*,jobject){return sampler().started()?JNI_TRUE:JNI_FALSE;}\nextern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeTestInstrumentEnabled(JNIEnv*,jobject){return sampler().testInstrumentEnabled()?JNI_TRUE:JNI_FALSE;}\nextern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeTestNoteOn(JNIEnv*,jobject,jint note,jint velocity){return sampler().testNoteOn(static_cast<std::uint8_t>(std::clamp(note,0,127)),static_cast<std::uint8_t>(std::clamp(velocity,0,127)))?JNI_TRUE:JNI_FALSE;}\nextern "C" JNIEXPORT void JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeTestNoteOff(JNIEnv*,jobject,jint note){sampler().testNoteOff(static_cast<std::uint8_t>(std::clamp(note,0,127)));}\n'''
if 'nativeMidiActivitySerial' not in s:
    if jni_anchor not in s: raise SystemExit('JNI diagnostics anchor changed')
    s=s.replace(jni_anchor,jni_new,1)
cpp.write_text(s,encoding='utf-8')

k=native.read_text(encoding='utf-8')
anchor='''    external fun nativeMidiLastError(): Int\n'''
add='''    external fun nativeMidiLastError(): Int\n    external fun nativeMidiActivitySerial(): Long\n    external fun nativeMidiLastMessage(): IntArray\n    external fun nativeAudioStarted(): Boolean\n    external fun nativeTestInstrumentEnabled(): Boolean\n    external fun nativeTestNoteOn(note: Int, velocity: Int): Boolean\n    external fun nativeTestNoteOff(note: Int)\n'''
if 'nativeMidiActivitySerial()' not in k:
    if anchor not in k: raise SystemExit('NativeSamplerEngine MIDI anchor changed')
    k=k.replace(anchor,add,1)
native.write_text(k,encoding='utf-8')

k=controller.read_text(encoding='utf-8')
old='''        val droppedRealtimeEvents: Int\n    )\n'''
new='''        val droppedRealtimeEvents: Int,\n        val running: Boolean,\n        val lastError: Int,\n        val testInstrumentEnabled: Boolean\n    )\n\n    data class MidiStatus(\n        val deviceCount: Int,\n        val lastError: Int,\n        val activitySerial: Long,\n        val status: Int,\n        val data1: Int,\n        val data2: Int\n    )\n'''
if old in k:
    k=k.replace(old,new,1)
elif 'data class MidiStatus(' not in k:
    raise SystemExit('AudioStatus anchor changed')

old='''            limiterGainReductionDb = meters.getOrElse(2) { 0f },\n            activeVoices = meters.getOrElse(3) { 0f }.toInt(),\n            droppedRealtimeEvents = meters.getOrElse(4) { 0f }.toInt()\n        )\n    }\n'''
new='''            limiterGainReductionDb = meters.getOrElse(2) { 0f },\n            activeVoices = meters.getOrElse(3) { 0f }.toInt(),\n            droppedRealtimeEvents = meters.getOrElse(4) { 0f }.toInt(),\n            running = engine.nativeAudioStarted(),\n            lastError = engine.nativeLastAudioError(),\n            testInstrumentEnabled = engine.nativeTestInstrumentEnabled()\n        )\n    }\n\n    fun midiStatus(): MidiStatus {\n        val msg = engine.nativeMidiLastMessage()\n        return MidiStatus(\n            deviceCount = engine.nativeMidiDeviceCount().coerceAtLeast(0),\n            lastError = engine.nativeMidiLastError(),\n            activitySerial = engine.nativeMidiActivitySerial(),\n            status = msg.getOrElse(0) { 0 },\n            data1 = msg.getOrElse(1) { 0 },\n            data2 = msg.getOrElse(2) { 0 }\n        )\n    }\n\n    fun testEpNoteOn(note: Int = 60, velocity: Int = 110): Boolean =\n        !closed.get() && engine.nativeTestNoteOn(note.coerceIn(0,127), velocity.coerceIn(1,127))\n\n    fun testEpNoteOff(note: Int = 60) {\n        if (!closed.get()) engine.nativeTestNoteOff(note.coerceIn(0,127))\n    }\n'''
if old in k:
    k=k.replace(old,new,1)
elif 'fun midiStatus(): MidiStatus' not in k:
    raise SystemExit('audioStatus return anchor changed')
controller.write_text(k,encoding='utf-8')

k=view.read_text(encoding='utf-8')
field_anchor='''    private var frameAudioStatus: SamplerAppController.AudioStatus = sampler.audioStatus()\n    private var frameUpperMeters: List<Pair<Float, Float>> = List(7) { 0f to 0f }\n'''
field_new='''    private var frameAudioStatus: SamplerAppController.AudioStatus = sampler.audioStatus()\n    private var frameUpperMeters: List<Pair<Float, Float>> = List(7) { 0f to 0f }\n    private var frameMidiStatus: SamplerAppController.MidiStatus = sampler.midiStatus()\n    private var lastMidiSerial = frameMidiStatus.activitySerial\n    private var midiFlashUntilMs = 0L\n'''
if 'frameMidiStatus:' not in k:
    if field_anchor not in k: raise SystemExit('view diagnostic field anchor changed')
    k=k.replace(field_anchor,field_new,1)

old='''        frameAudioStatus = sampler.audioStatus()\n        frameUpperMeters = sampler.upperMeters()\n\n        val w = width.toFloat()\n'''
new='''        frameAudioStatus = sampler.audioStatus()\n        frameUpperMeters = sampler.upperMeters()\n        frameMidiStatus = sampler.midiStatus()\n        if (frameMidiStatus.activitySerial != lastMidiSerial) {\n            lastMidiSerial = frameMidiStatus.activitySerial\n            midiFlashUntilMs = android.os.SystemClock.elapsedRealtime() + 220L\n        }\n\n        val w = width.toFloat()\n'''
if old in k:
    k=k.replace(old,new,1)
elif 'midiFlashUntilMs = android.os.SystemClock.elapsedRealtime() + 220L' not in k:
    raise SystemExit('view onDraw diagnostic anchor changed')

def replace_method(text,start_sig,next_sig,body):
    a=text.find(start_sig)
    if a<0: raise SystemExit(f'missing {start_sig}')
    b=text.find(next_sig,a)
    if b<0: raise SystemExit(f'missing next after {start_sig}')
    return text[:a]+body.rstrip()+'\n\n'+text[b:]

header=r'''    private fun drawHeader(c: Canvas, w: Float, h: Float) {
        val sx = w / DESIGN_W
        val sy = h / DESIGN_H
        val ss = min(sx, sy)
        val hh = 76f * sy
        paint.shader = LinearGradient(0f, 0f, 0f, hh, C_BG_TOP, C_BG, Shader.TileMode.CLAMP)
        c.drawRect(0f, 0f, w, hh, paint)
        paint.shader = null
        line(c, 16f*sx, hh-1f*sy, w-16f*sx, hh-1f*sy, Color.argb(28,255,255,255), 1f*ss)
        val gear=RectF(16f*sx,12f*sy,84f*sx,64f*sy)
        silvKey(c,gear,page==Page.SETTINGS,false); drawGear(c,gear.centerX(),gear.centerY(),12f*ss)
        hit(gear,{_,_-> page=Page.SETTINGS; refreshGlobal()})
        val panic=RectF(96f*sx,12f*sy,244f*sx,64f*sy)
        silvKey(c,panic,false,true); silvLed(c,panic.left+20f*sx,panic.centerY(),true,true,5.5f*ss)
        text(c,"PANIC",panic.centerX()+8f*sx,panic.centerY()+6f*sy,17f*ss,C_TEXT,alignCenter=true,bold=true)
        hit(panic,{_,_->sampler.panic()})
        val test=RectF(254f*sx,12f*sy,374f*sx,64f*sy)
        val testReady=frameAudioStatus.testInstrumentEnabled
        silvKey(c,test,testReady,false,enabled=testReady); silvLed(c,test.left+18f*sx,test.centerY(),testReady,false,4.5f*ss)
        text(c,"TEST EP",test.centerX()+7f*sx,test.centerY()+6f*sy,14f*ss,if(testReady)C_TEXT else C_TEXT_MUTED,alignCenter=true,bold=testReady)
        if(testReady) hit(test,{_,_-> if(sampler.testEpNoteOn(60,110)) postDelayed({sampler.testEpNoteOff(60)},650L)})
        drawBrand(c,w*0.5f,38f*sy,sx,sy)
        val a=frameAudioStatus; val m=frameMidiStatus; val now=android.os.SystemClock.elapsedRealtime()
        val midiRx=now<midiFlashUntilMs; val midiConnected=max(midiConnections,m.deviceCount)>0
        val midiValue=when { midiRx&&(m.status and 0xF0)==0x90&&m.data2>0 -> "RX ${midiNoteName(m.data1)}"; midiRx -> "RX"; midiConnected -> "ON"; m.lastError!=0 -> "E${-m.lastError}"; else -> "OFF" }
        val audioOk=a.running&&a.sampleRate==96000&&a.framesPerBurst==96&&a.bufferSizeFrames==192
        val audioValue=when { audioOk -> "96k 96x2"; a.running -> "${a.sampleRate/1000}k ${a.framesPerBurst}"; a.lastError!=0 -> "OFF E${-a.lastError}"; else -> "OFF" }
        val chipY=16f*sy; val chipH=44f*sy; val gap=8f*sx; var right=w-16f*sx
        val midiR=RectF(right-124f*sx,chipY,right,chipY+chipH); silvStatus(c,midiR,"MIDI",midiValue,midiConnected||midiRx,sx,sy); right=midiR.left-gap
        val xrunR=RectF(right-108f*sx,chipY,right,chipY+chipH); silvStatus(c,xrunR,"XRUN",a.xrunCount.toString(),audioOk&&a.xrunCount==0,sx,sy); right=xrunR.left-gap
        val audioR=RectF(right-164f*sx,chipY,right,chipY+chipH); silvStatus(c,audioR,"AUDIO",audioValue,audioOk,sx,sy)
    }'''
k=replace_method(k,'    private fun drawHeader(','    private fun drawRightNav(',header)
k=k.replace('''        text(c, activeItem?.name ?: "Силветон", left+95f*sx, nowBottom-17f*sy, 42f*ss, C_TEXT)\n''','''        val currentName = activeItem?.name ?: if (frameAudioStatus.testInstrumentEnabled) "TEST EP" else "Силветон"\n        text(c, currentName, left+95f*sx, nowBottom-17f*sy, 42f*ss, C_TEXT)\n''',1)
k=k.replace('''        text(c, if (activeItem != null) "%03d".format(currentNumber) else "–", left, nowBottom-16f*sy, 46f*ss, C_ACCENT)\n''','''        text(c, if (activeItem != null) "%03d".format(currentNumber) else if (frameAudioStatus.testInstrumentEnabled) "TST" else "–", left, nowBottom-16f*sy, 46f*ss, C_ACCENT)\n''',1)
status=r'''    private fun silvStatus(c: Canvas, r: RectF, label: String, value: String, on: Boolean, sx: Float, sy: Float) {
        val ss=min(sx,sy); paint.style=Paint.Style.FILL
        paint.shader=LinearGradient(0f,r.top,0f,r.bottom,Color.rgb(27,33,42),Color.rgb(18,23,30),Shader.TileMode.CLAMP); c.drawRoundRect(r,r.height()*0.5f,r.height()*0.5f,paint); paint.shader=null
        paint.style=Paint.Style.STROKE; paint.strokeWidth=1f*ss; paint.color=Color.argb(38,255,255,255); c.drawRoundRect(r,r.height()*0.5f,r.height()*0.5f,paint); paint.style=Paint.Style.FILL
        if(label=="MIDI") { silvLed(c,r.left+17f*sx,r.centerY(),on,false,5f*ss); text(c,label,r.left+30f*sx,r.centerY()-2f*sy,9f*ss,C_TEXT_MUTED,bold=true); text(c,value,r.left+30f*sx,r.centerY()+14f*sy,13f*ss,if(on)C_TEXT else C_PANIC,bold=true) }
        else { text(c,label,r.left+13f*sx,r.centerY()-3f*sy,9f*ss,C_TEXT_MUTED,bold=true); text(c,value,r.left+13f*sx,r.centerY()+14f*sy,14f*ss,if(on)C_TEXT else C_PANIC,bold=true) }
    }'''
k=replace_method(k,'    private fun silvStatus(','    private fun colorWithAlpha(',status)
anchor='    private fun colorWithAlpha(color: Int, alpha: Int): Int = Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color))\n'
helper='''    private fun midiNoteName(note: Int): String {\n        val n=note.coerceIn(0,127); val names=arrayOf("C","C#","D","D#","E","F","F#","G","G#","A","A#","B")\n        return names[n%12]+(n/12-1)\n    }\n\n'''
if 'private fun midiNoteName(' not in k:
    if anchor not in k: raise SystemExit('view helper anchor changed')
    k=k.replace(anchor,helper+anchor,1)
view.write_text(k,encoding='utf-8')

for path, needles in {
    cpp:['midiLastStatus_.store','midiActivitySerial() const noexcept','testNoteOn(std::uint8_t note','nativeMidiActivitySerial','nativeAudioStarted','nativeTestNoteOn'],
    native:['nativeMidiActivitySerial(): Long','nativeAudioStarted(): Boolean','nativeTestNoteOn(note: Int, velocity: Int)'],
    controller:['data class MidiStatus(','running = engine.nativeAudioStarted()','fun testEpNoteOn(','fun midiStatus(): MidiStatus'],
    view:['"TEST EP"','"AUDIO"','"XRUN"','"RX ${midiNoteName(m.data1)}"','frameMidiStatus = sampler.midiStatus()']
}.items():
    text=path.read_text(encoding='utf-8')
    for n in needles:
        if n not in text: raise SystemExit(f'live diagnostics contract missing in {path.name}: {n}')
print('PASS: live hardware diagnostics + TEST EP control installed (real AUDIO/XRUN/MIDI state, MIDI RX activity, audible C4 self-test)')
