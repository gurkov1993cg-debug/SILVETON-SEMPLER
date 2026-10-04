from pathlib import Path
import sys

root = Path(sys.argv[1])
java_dir = root / 'android/app/src/main/java/com/silveton/sampler'
runtime = java_dir / 'SamplerRuntime.kt'
root_mode = java_dir / 'SilvetonRootRuntime.kt'
activity = java_dir / 'MainActivity.kt'
for p in (runtime, root_mode, activity):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

s = runtime.read_text(encoding='utf-8')
s = s.replace('import android.os.Handler\nimport android.os.Looper\n',
              'import android.os.Handler\nimport android.os.HandlerThread\nimport android.os.Process\n')
s = s.replace(
'''    private val handler = Handler(Looper.getMainLooper())\n    private var running = false\n''',
'''    private val recoveryThread = HandlerThread(\n        "SilvetonAudioRecovery",\n        Process.THREAD_PRIORITY_BACKGROUND\n    ).apply { start() }\n    private val handler = Handler(recoveryThread.looper)\n    private var running = false\n''')

s = s.replace(
'''        val root = SilvetonRootRuntime.enter()\n        if (!root.ok) {\n            statusChanged(root.message)\n            return false\n        }\n        if (!running) {\n''',
'''        val root = SilvetonRootRuntime.enter()\n        if (!root.ok) {\n            statusChanged(root.message)\n            return false\n        }\n        val audioPermissions = SilvetonRootRuntime.refreshAudioPermissions()\n        if (!audioPermissions.ok) {\n            statusChanged(audioPermissions.message)\n            return false\n        }\n        if (!running) {\n''', 1)

s = s.replace(
'''                } else {\n                    val error = engine.nativeLastAudioError()\n                    if (!engine.nativeAudioRecover()) {\n''',
'''                } else {\n                    val perms = SilvetonRootRuntime.refreshAudioPermissions()\n                    if (!perms.ok) {\n                        statusChanged(perms.message)\n                        handler.postDelayed(this, 250L)\n                        return\n                    }\n                    val error = engine.nativeLastAudioError()\n                    if (!engine.nativeAudioRecover()) {\n''', 1)

resume_anchor = '''    fun resumeAudio(): Boolean {\n        if (closed) return false\n        val root = SilvetonRootRuntime.enter()\n        if (!root.ok) {\n            statusChanged(root.message)\n            return false\n        }\n'''
resume_new = '''    fun resumeAudio(): Boolean {\n        if (closed) return false\n        val root = SilvetonRootRuntime.enter()\n        if (!root.ok) {\n            statusChanged(root.message)\n            return false\n        }\n        val audioPermissions = SilvetonRootRuntime.refreshAudioPermissions()\n        if (!audioPermissions.ok) {\n            statusChanged(audioPermissions.message)\n            return false\n        }\n'''
if resume_anchor in s:
    s = s.replace(resume_anchor, resume_new, 1)
elif 'val audioPermissions = SilvetonRootRuntime.refreshAudioPermissions()' not in s[s.find('fun resumeAudio'):]:
    raise SystemExit('resumeAudio anchor changed')

s = s.replace(
'''        handler.removeCallbacks(recoveryPoll)\n        engine.nativePanic()\n''',
'''        handler.removeCallbacks(recoveryPoll)\n        recoveryThread.quitSafely()\n        engine.nativePanic()\n''', 1)
runtime.write_text(s, encoding='utf-8')

k = root_mode.read_text(encoding='utf-8')
insert_before = '    fun refreshMidiPermissions(): Result {\n'
refresh_audio = r'''    fun refreshAudioPermissions(): Result {
        val uid = Process.myUid()
        val script = """
            for n in /dev/snd/controlC0 /dev/snd/pcmC0D0p; do
                [ -e "${'$'}n" ] || { echo "Missing ${'$'}n"; exit 51; }
                chown ${uid}:1005 "${'$'}n" 2>/dev/null || exit 52
                chmod 0660 "${'$'}n" 2>/dev/null || exit 53
            done
            exit 0
        """.trimIndent()
        return runSu(script, 5)
    }

'''
if 'fun refreshAudioPermissions()' not in k:
    if insert_before not in k:
        raise SystemExit('root refresh insertion anchor changed')
    k = k.replace(insert_before, refresh_audio + insert_before, 1)

start = k.find('    private fun runSu(script: String, timeoutSeconds: Long): Result = try {')
if start < 0:
    raise SystemExit('runSu block missing')
end = k.find('\n    }\n}', start)
if end < 0:
    raise SystemExit('runSu block end missing')
new = r'''    private fun runSu(script: String, timeoutSeconds: Long): Result = try {
        val p = ProcessBuilder("su", "-c", script)
            .redirectErrorStream(true)
            .start()
        if (!p.waitFor(timeoutSeconds, TimeUnit.SECONDS)) {
            p.destroyForcibly()
            p.waitFor(1, TimeUnit.SECONDS)
            Result(false, "Root setup timed out")
        } else {
            val output = p.inputStream.bufferedReader().use { it.readText() }.trim()
            if (p.exitValue() == 0) Result(true)
            else Result(false, "Root setup failed (${p.exitValue()}): $output")
        }
    } catch (t: Throwable) {
        Result(false, "Root unavailable: ${t.message ?: t.javaClass.simpleName}")
    }
'''
k = k[:start] + new + k[end+6:]
root_mode.write_text(k, encoding='utf-8')

a = activity.read_text(encoding='utf-8')
if 'java.util.concurrent.Executors' not in a:
    a = a.replace('import androidx.core.view.WindowInsetsControllerCompat\n',
                  'import androidx.core.view.WindowInsetsControllerCompat\nimport java.util.concurrent.Executors\n')
a = a.replace(
'''    private lateinit var samplerView: SilvetonSamplerView\n    private var created = false\n''',
'''    private lateinit var samplerView: SilvetonSamplerView\n    private val audioLifecycle = Executors.newSingleThreadExecutor { task ->\n        Thread(task, "SilvetonAudioLifecycle").apply { priority = Thread.NORM_PRIORITY }\n    }\n    private var created = false\n''')
a = a.replace(
'''    override fun onStart() {\n        super.onStart()\n        if (created && !sampler.start()) {\n            samplerView.setWarning(sampler.engine.nativeLastTextError().ifBlank { "Sampler start failed" })\n        }\n    }\n''',
'''    override fun onStart() {\n        super.onStart()\n        if (created) audioLifecycle.execute { sampler.start() }\n    }\n''')
a = a.replace('        if (created) sampler.resumeAudio()\n',
              '        if (created) audioLifecycle.execute { sampler.resumeAudio() }\n', 1)
a = a.replace(
'''    override fun onPause() {\n        if (created) sampler.pauseAudio()\n        super.onPause()\n    }\n''',
'''    override fun onPause() {\n        if (created) audioLifecycle.execute { sampler.pauseAudio() }\n        super.onPause()\n    }\n''')
a = a.replace(
'''    override fun onDestroy() {\n        if (created) sampler.close()\n        super.onDestroy()\n    }\n''',
'''    override fun onDestroy() {\n        if (created) audioLifecycle.execute { sampler.close() }\n        audioLifecycle.shutdown()\n        super.onDestroy()\n    }\n''')
activity.write_text(a, encoding='utf-8')

checks = {
    runtime: ['"SilvetonAudioRecovery"', 'SilvetonRootRuntime.refreshAudioPermissions()', 'recoveryThread.quitSafely()'],
    root_mode: ['fun refreshAudioPermissions(): Result', 'if (!p.waitFor(timeoutSeconds, TimeUnit.SECONDS))', 'val output = p.inputStream.bufferedReader().use { it.readText() }.trim()'],
    activity: ['SilvetonAudioLifecycle', 'audioLifecycle.execute { sampler.start() }', 'audioLifecycle.execute { sampler.resumeAudio() }', 'audioLifecycle.execute { sampler.pauseAudio() }'],
}
for p, needles in checks.items():
    text = p.read_text(encoding='utf-8')
    for needle in needles:
        if needle not in text:
            raise SystemExit(f'final stability contract missing in {p.name}: {needle}')
if 'Handler(Looper.getMainLooper())' in runtime.read_text(encoding='utf-8'):
    raise SystemExit('audio recovery still bound to UI looper')
print('PASS: final runtime stability fix applied (off-UI ALSA/root lifecycle + audio-node refresh + bounded su timeout)')
