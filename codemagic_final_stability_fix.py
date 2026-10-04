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

# 1) Recovery must never run on the UI looper.
s = runtime.read_text(encoding='utf-8')
s = s.replace('import android.os.Handler\nimport android.os.Looper\n',
              'import android.os.Handler\nimport android.os.HandlerThread\nimport android.os.Process\n')
s = s.replace(
'''    private val handler = Handler(Looper.getMainLooper())\n    private var running = false\n''',
'''    private val recoveryThread = HandlerThread(\n        "SilvetonAudioRecovery",\n        Process.THREAD_PRIORITY_BACKGROUND\n    ).apply { start() }\n    private val handler = Handler(recoveryThread.looper)\n    private var running = false\n''')

# Refresh DAC ownership before every open/reopen/recovery. enter() deliberately
# becomes a no-op after the first successful root setup, but /dev/snd nodes can
# be recreated by Android/hot-plug and lose our per-app ownership.
s = s.replace(
'''        val root = SilvetonRootRuntime.enter()\n        if (!root.ok) {\n            statusChanged(root.message)\n            return false\n        }\n        if (!running) {\n''',
'''        val root = SilvetonRootRuntime.enter()\n        if (!root.ok) {\n            statusChanged(root.message)\n            return false\n        }\n        val audioPermissions = SilvetonRootRuntime.refreshAudioPermissions()\n        if (!audioPermissions.ok) {\n            statusChanged(audioPermissions.message)\n            return false\n        }\n        if (!running) {\n''', 1)

# Recovery path: refresh node ownership before touching PCM/control again.
s = s.replace(
'''                } else {\n                    val error = engine.nativeLastAudioError()\n                    if (!engine.nativeAudioRecover()) {\n''',
'''                } else {\n                    val perms = SilvetonRootRuntime.refreshAudioPermissions()\n                    if (!perms.ok) {\n                        statusChanged(perms.message)\n                        handler.postDelayed(this, 250L)\n                        return\n                    }\n                    val error = engine.nativeLastAudioError()\n                    if (!engine.nativeAudioRecover()) {\n''', 1)

# Resume path has a second enter() occurrence.
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

# 2) Root helper: explicit audio-node refresh + real timeout semantics.
k = root_mode.read_text(encoding='utf-8')
insert_before = '    fun refreshMidiPermissions(): Result {\n'
refresh_audio = r'''    fun refreshAudioPermissions(): Result {
        val uid = Process.myUid()
        val script = """
            for n in /dev/snd/controlC0 /dev/snd/pcmC0D0p; do
                [ -e "${'$'}n" ] || { echo "Missing ${'$'}n"; exit 51; }
                chown ''' + '${uid}' + r''':1005 "${'$'}n" 2>/dev/null || exit 52
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
old = k[start:end+6]
new = r'''    private fun runSu(script: String, timeoutSeconds: Long): Result = try {
        val p = ProcessBuilder("su", "-c", script)
            .redirectErrorStream(true)
            .start()

        // Never call readText() before the process has exited: if su or a shell
        // command hangs, readText blocks forever and makes the timeout useless.
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

# 3) All root/ALSA lifecycle work runs on one dedicated executor, never UI.
a = activity.read_text(encoding='utf-8')
if 'import java.util.concurrent.Executors' not in a:
    import_anchor = 'import androidx.core.view.WindowInsetsControllerCompat\n'
    if import_anchor in a:
        a = a.replace(import_anchor, import_anchor + 'import java.util.concurrent.Executors\n', 1)
    else:
        bundle_anchor = 'import android.os.Bundle\n'
        if bundle_anchor not in a:
            raise SystemExit('MainActivity import anchor changed')
        a = a.replace(bundle_anchor, bundle_anchor + 'import java.util.concurrent.Executors\n', 1)

if 'private val audioLifecycle = Executors.newSingleThreadExecutor' not in a:
    field_anchor = '    private lateinit var samplerView: SilvetonSamplerView\n'
    if field_anchor not in a:
        raise SystemExit('MainActivity samplerView field anchor changed')
    executor = '''    private val audioLifecycle = Executors.newSingleThreadExecutor { task ->\n        Thread(task, "SilvetonAudioLifecycle").apply { priority = Thread.NORM_PRIORITY }\n    }\n'''
    a = a.replace(field_anchor, field_anchor + executor, 1)

def replace_activity_method(text: str, start_sig: str, next_sig: str, body: str) -> str:
    begin = text.find(start_sig)
    if begin < 0:
        raise SystemExit(f'MainActivity method missing: {start_sig.strip()}')
    finish = text.find(next_sig, begin)
    if finish < 0:
        raise SystemExit(f'MainActivity next method missing after: {start_sig.strip()}')
    return text[:begin] + body.rstrip() + '\n\n' + text[finish:]

a = replace_activity_method(
    a,
    '    override fun onStart() {',
    '    override fun onResume() {',
    '''    override fun onStart() {\n        super.onStart()\n        if (created) audioLifecycle.execute { sampler.start() }\n    }'''
)
a = replace_activity_method(
    a,
    '    override fun onResume() {',
    '    override fun onPause() {',
    '''    override fun onResume() {\n        super.onResume()\n        hideSystemUi()\n        if (created) audioLifecycle.execute { sampler.resumeAudio() }\n    }'''
)
a = replace_activity_method(
    a,
    '    override fun onPause() {',
    '    override fun onDestroy() {',
    '''    override fun onPause() {\n        if (created) audioLifecycle.execute { sampler.pauseAudio() }\n        super.onPause()\n    }'''
)
a = replace_activity_method(
    a,
    '    override fun onDestroy() {',
    '    override fun onWindowFocusChanged(',
    '''    override fun onDestroy() {\n        if (created) audioLifecycle.execute { sampler.close() }\n        audioLifecycle.shutdown()\n        super.onDestroy()\n    }'''
)
activity.write_text(a, encoding='utf-8')

# Contracts.
checks = {
    runtime: [
        '"SilvetonAudioRecovery"',
        'SilvetonRootRuntime.refreshAudioPermissions()',
        'recoveryThread.quitSafely()',
    ],
    root_mode: [
        'fun refreshAudioPermissions(): Result',
        'if (!p.waitFor(timeoutSeconds, TimeUnit.SECONDS))',
        'val output = p.inputStream.bufferedReader().use { it.readText() }.trim()',
    ],
    activity: [
        'SilvetonAudioLifecycle',
        'audioLifecycle.execute { sampler.start() }',
        'audioLifecycle.execute { sampler.resumeAudio() }',
        'audioLifecycle.execute { sampler.pauseAudio() }',
    ],
}
for p, needles in checks.items():
    text = p.read_text(encoding='utf-8')
    for needle in needles:
        if needle not in text:
            raise SystemExit(f'final stability contract missing in {p.name}: {needle}')
if 'Handler(Looper.getMainLooper())' in runtime.read_text(encoding='utf-8'):
    raise SystemExit('audio recovery still bound to UI looper')

# 4) Final UI/hardware diagnostics. This runs after the LIVE MIDI visual skin,
# so the skin cannot overwrite the real AUDIO/XRUN/MIDI indicators or TEST EP.
diag = Path(__file__).with_name('codemagic_live_diagnostics_fix.py')
if not diag.is_file():
    raise SystemExit(f'missing diagnostics patch: {diag}')
exec(compile(diag.read_text(encoding='utf-8'), str(diag), 'exec'), {'__name__':'__main__', '__file__':str(diag)})

print('PASS: final runtime stability fix applied (off-UI ALSA/root lifecycle + audio-node refresh + bounded su timeout + live diagnostics)')
