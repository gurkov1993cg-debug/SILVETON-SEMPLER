from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
runtime = root / 'android/app/src/main/java/com/silveton/sampler/SamplerRuntime.kt'
controller = root / 'android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt'
midi = root / 'android/app/src/main/java/com/silveton/sampler/SamplerMidiController.kt'
root_mode = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'
for p in (cpp, runtime, controller, midi, root_mode):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

s = cpp.read_text(encoding='utf-8')
old = '''    bool open() {\n        std::lock_guard<std::mutex> lock(controlMutex_);\n        if (!openLocked()) return false;\n        return startDirectMidiLocked();\n    }\n'''
new = '''    bool open() {\n        std::lock_guard<std::mutex> lock(controlMutex_);\n        // MIDI is an independent realtime input path. Start it first so a\n        // temporary PCM failure never makes the keyboard disappear.\n        const bool midiOk = startDirectMidiLocked();\n        const bool audioOk = openLocked();\n        if (!audioOk) return false;\n        if (!midiOk) {\n            __android_log_print(ANDROID_LOG_WARN, kTag,\n                "Direct MIDI did not start while audio opened successfully");\n        }\n        return true;\n    }\n'''
if old in s:
    s = s.replace(old, new, 1)
elif 'temporary PCM failure never makes the keyboard disappear' not in s:
    raise SystemExit('C++ open() anchor changed')
cpp.write_text(s, encoding='utf-8')

s = controller.read_text(encoding='utf-8')
old = '''        if (closed.get()) return false\n        if (started.get()) return true\n        if (!runtime.start()) return false\n        midi.start()\n        started.set(true)\n'''
new = '''        if (closed.get()) return false\n        if (started.get()) return true\n        midi.start()\n        val audioOk = runtime.start()\n        if (!audioOk) return false\n        started.set(true)\n'''
if old in s:
    s = s.replace(old, new, 1)
elif 'val audioOk = runtime.start()' not in s:
    raise SystemExit('controller start block changed')
controller.write_text(s, encoding='utf-8')

s = runtime.read_text(encoding='utf-8')
s = s.replace('''        if (!engine.nativeAudioOpen()) {\n            statusChanged(engine.nativeLastTextError().ifBlank { "Audio open failed (${engine.nativeLastAudioError()})" })\n            SilvetonRootRuntime.leave()\n            return false\n        }\n''', '''        if (!engine.nativeAudioOpen()) {\n            statusChanged(engine.nativeLastTextError().ifBlank { "Audio open failed (${engine.nativeLastAudioError()})" })\n            // Keep root/SELinux grants while the Activity is alive: MIDI is\n            // independent and audio can be retried without tearing permissions down.\n            return false\n        }\n''')
s = s.replace('''        if (!engine.nativeAudioStart()) {\n            statusChanged(engine.nativeLastTextError().ifBlank { "Audio start failed (${engine.nativeLastAudioError()})" })\n            SilvetonRootRuntime.leave()\n            return false\n        }\n''', '''        if (!engine.nativeAudioStart()) {\n            statusChanged(engine.nativeLastTextError().ifBlank { "Audio start failed (${engine.nativeLastAudioError()})" })\n            return false\n        }\n''')
runtime.write_text(s, encoding='utf-8')

s = root_mode.read_text(encoding='utf-8')
anchor = '''    fun leave(): Result = synchronized(lock) {\n'''
method = r'''    fun refreshMidiPermissions(): Result {
        val uid = Process.myUid()
        val script = """
            for n in /dev/snd/midiC*D*; do
                [ -e "${'$'}n" ] || continue
                chown ${uid}:1005 "${'$'}n" 2>/dev/null || true
                chmod 0660 "${'$'}n" 2>/dev/null || true
            done
            exit 0
        """.trimIndent()
        return runSu(script)
    }

'''
if 'fun refreshMidiPermissions()' not in s:
    if anchor not in s:
        raise SystemExit('root leave anchor missing')
    s = s.replace(anchor, method + anchor, 1)
root_mode.write_text(s, encoding='utf-8')

s = midi.read_text(encoding='utf-8')
s = s.replace('''    private var lastError = 0\n''', '''    private var lastError = 0\n    private var lastPermissionRefreshMs = 0L\n''')
old = '''                if (error != 0) {\n                    statusChanged("Direct ALSA MIDI access failed (errno ${-error})")\n                } else if (previous != 0) {\n'''
new = '''                if (error != 0) {\n                    if (error == -13 || error == -1) {\n                        val now = android.os.SystemClock.elapsedRealtime()\n                        if (now - lastPermissionRefreshMs >= 1000L) {\n                            lastPermissionRefreshMs = now\n                            SilvetonRootRuntime.refreshMidiPermissions()\n                        }\n                    }\n                    statusChanged("Direct ALSA MIDI access failed (errno ${-error})")\n                } else if (previous != 0) {\n'''
if old in s:
    s = s.replace(old, new, 1)
elif 'refreshMidiPermissions()' not in s:
    raise SystemExit('MIDI status error block changed')
midi.write_text(s, encoding='utf-8')

checks = {
    cpp: ['const bool midiOk = startDirectMidiLocked();'],
    controller: ['midi.start()', 'val audioOk = runtime.start()'],
    root_mode: ['fun refreshMidiPermissions()'],
    midi: ['SilvetonRootRuntime.refreshMidiPermissions()'],
}
for path, needles in checks.items():
    text = path.read_text(encoding='utf-8')
    for needle in needles:
        if needle not in text:
            raise SystemExit(f'contract missing {needle!r} in {path}')
print('PASS: MIDI/audio startup decoupled + hotplug MIDI permissions')
