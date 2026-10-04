from pathlib import Path
import sys

root = Path(sys.argv[1])
java_dir = root / 'android/app/src/main/java/com/silveton/sampler'
runtime = java_dir / 'SamplerRuntime.kt'
system_mode = java_dir / 'SilvetonSystemMode.kt'
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
manifest = root / 'android/app/src/main/AndroidManifest.xml'

for p in (runtime, cpp, manifest):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

system_mode.write_text(r'''package com.silveton.sampler

/**
 * Switches the dedicated SILVETON system image into and out of live-audio mode.
 * The property is accepted only by the SILVETON SELinux domain in the system image.
 * Development/user APKs simply continue without the system optimization if setprop is denied.
 */
internal object SilvetonSystemMode {
    private const val PROPERTY = "sys.silveton.performance"
    private val lock = Any()
    @Volatile private var active = false

    fun enter(): Boolean = synchronized(lock) {
        if (active) return@synchronized true
        val ok = setProperty("1")
        if (ok) active = true
        ok
    }

    fun leave() = synchronized(lock) {
        if (!active) return@synchronized
        setProperty("0")
        active = false
    }

    private fun setProperty(value: String): Boolean = try {
        val process = ProcessBuilder("/system/bin/setprop", PROPERTY, value)
            .redirectErrorStream(true)
            .start()
        process.inputStream.bufferedReader().use { it.readText() }
        process.waitFor() == 0
    } catch (_: Throwable) {
        false
    }
}
''', encoding='utf-8')

runtime.write_text(r'''package com.silveton.sampler

import android.os.Handler
import android.os.Looper

/** Lifecycle owner for the Direct-ALSA sampler stream. */
class SamplerRuntime(
    val engine: NativeSamplerEngine = NativeSamplerEngine(),
    private val statusChanged: (String?) -> Unit = {}
) : AutoCloseable {
    private val handler = Handler(Looper.getMainLooper())
    private var running = false
    private var closed = false

    private val recoveryPoll = object : Runnable {
        override fun run() {
            if (closed) return
            if (engine.nativeAudioNeedsRecovery()) {
                val error = engine.nativeLastAudioError()
                if (!engine.nativeAudioRecover()) {
                    statusChanged("Audio recovery failed: $error ${engine.nativeLastTextError()}")
                } else if (!engine.nativeAudioStart()) {
                    statusChanged(engine.nativeLastTextError().ifBlank { "Audio restart failed (${engine.nativeLastAudioError()})" })
                } else {
                    statusChanged(null)
                }
            }
            handler.postDelayed(this, 500L)
        }
    }

    fun start(): Boolean {
        if (closed) return false
        SilvetonSystemMode.enter()
        if (!running) {
            running = true
            handler.post(recoveryPoll)
        }
        if (!engine.nativeAudioOpen()) {
            SilvetonSystemMode.leave()
            statusChanged(engine.nativeLastTextError().ifBlank { "Audio open failed (${engine.nativeLastAudioError()})" })
            return false
        }
        if (!engine.nativeAudioStart()) {
            SilvetonSystemMode.leave()
            statusChanged(engine.nativeLastTextError().ifBlank { "Audio start failed (${engine.nativeLastAudioError()})" })
            return false
        }
        statusChanged(null)
        return true
    }

    fun suspendAudio() {
        if (closed) return
        engine.nativeAudioStop()
        SilvetonSystemMode.leave()
    }

    fun resumeAudio(): Boolean {
        if (closed) return false
        SilvetonSystemMode.enter()
        if (engine.nativeAudioNeedsRecovery() && !engine.nativeAudioRecover()) {
            SilvetonSystemMode.leave()
            return false
        }
        val ok = engine.nativeAudioStart()
        if (!ok) SilvetonSystemMode.leave()
        return ok
    }

    override fun close() {
        if (closed) return
        closed = true
        running = false
        handler.removeCallbacks(recoveryPoll)
        engine.nativePanic()
        engine.nativeAudioStop()
        engine.nativeAudioClose()
        SilvetonSystemMode.leave()
    }
}
''', encoding='utf-8')

s = cpp.read_text(encoding='utf-8')
if '#include <sched.h>' not in s:
    s = s.replace('#include <sys/resource.h>\n', '#include <sys/resource.h>\n#include <sched.h>\n')

helper_anchor = 'constexpr const char* kTag = "SilvetonSampler";\n\n'
helper = r'''constexpr const char* kTag = "SilvetonSampler";

static void pinCurrentThreadToCpu(int cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)::sched_setaffinity(0, sizeof(set), &set);
}

'''
if 'pinCurrentThreadToCpu' not in s:
    if helper_anchor not in s:
        raise SystemExit('C++ helper insertion point changed')
    s = s.replace(helper_anchor, helper)

s = s.replace(
    '    void directMidiLoop() noexcept {\n        (void)::setpriority(PRIO_PROCESS, 0, -16);\n',
    '    void directMidiLoop() noexcept {\n        pinCurrentThreadToCpu(6);\n        (void)::setpriority(PRIO_PROCESS, 0, -16);\n'
)
s = s.replace(
    '    void audioLoop() noexcept {\n        (void)::setpriority(PRIO_PROCESS, 0, -16);\n',
    '    void audioLoop() noexcept {\n        pinCurrentThreadToCpu(7);\n        (void)::setpriority(PRIO_PROCESS, 0, -16);\n'
)
if 'pinCurrentThreadToCpu(6);' not in s or 'pinCurrentThreadToCpu(7);' not in s:
    raise SystemExit('failed to pin Direct MIDI/audio threads')
cpp.write_text(s, encoding='utf-8')

m = manifest.read_text(encoding='utf-8')
permission_block = '''    <permission\n        android:name="com.silveton.sampler.permission.DIRECT_ALSA"\n        android:protectionLevel="signature" />\n    <uses-permission android:name="com.silveton.sampler.permission.DIRECT_ALSA" />\n'''
if 'com.silveton.sampler.permission.DIRECT_ALSA' not in m:
    m = m.replace('<manifest xmlns:android="http://schemas.android.com/apk/res/android">\n',
                  '<manifest xmlns:android="http://schemas.android.com/apk/res/android">\n' + permission_block)
if 'android:directBootAware=' not in m:
    m = m.replace('        android:allowBackup="false"\n',
                  '        android:allowBackup="false"\n        android:directBootAware="true"\n')
manifest.write_text(m, encoding='utf-8')

checks = {
    'system mode source': system_mode,
    'runtime': runtime,
    'cpp': cpp,
    'manifest': manifest,
}
for name, path in checks.items():
    if not path.is_file() or path.stat().st_size == 0:
        raise SystemExit(f'{name} was not generated')

print('PASS: SILVETON system-app live mode + CPU isolation hooks installed')
