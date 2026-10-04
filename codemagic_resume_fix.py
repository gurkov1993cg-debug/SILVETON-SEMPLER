from pathlib import Path
import sys
root=Path(sys.argv[1])
runtime=root/'android/app/src/main/java/com/silveton/sampler/SamplerRuntime.kt'
controller=root/'android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt'
for p in (runtime,controller):
    if not p.is_file(): raise SystemExit(f'missing {p}')

s=runtime.read_text(encoding='utf-8')
old='''    fun resumeAudio(): Boolean {\n        if (closed) return false\n        val root = SilvetonRootRuntime.enter()\n        if (!root.ok) {\n            statusChanged(root.message)\n            return false\n        }\n        if (engine.nativeAudioNeedsRecovery() && !engine.nativeAudioRecover()) {\n            statusChanged(engine.nativeLastTextError())\n            return false\n        }\n        val ok = engine.nativeAudioStart()\n        if (!ok) {\n            statusChanged(engine.nativeLastTextError())\n        } else {\n            promoteThreadsAsync()\n            statusChanged(null)\n        }\n        return ok\n    }\n'''
new='''    fun resumeAudio(): Boolean {\n        if (closed) return false\n        val root = SilvetonRootRuntime.enter()\n        if (!root.ok) {\n            statusChanged(root.message)\n            return false\n        }\n        if (engine.nativeAudioNeedsRecovery() && !engine.nativeAudioRecover()) {\n            statusChanged(engine.nativeLastTextError().ifBlank { "Audio recovery failed (${engine.nativeLastAudioError()})" })\n            return false\n        }\n        if (!engine.nativeAudioOpen()) {\n            statusChanged(engine.nativeLastTextError().ifBlank { "Audio reopen failed (${engine.nativeLastAudioError()})" })\n            return false\n        }\n        val ok = engine.nativeAudioStart()\n        if (!ok) {\n            statusChanged(engine.nativeLastTextError().ifBlank { "Audio start failed (${engine.nativeLastAudioError()})" })\n        } else {\n            promoteThreadsAsync()\n            statusChanged(null)\n        }\n        return ok\n    }\n'''
if old in s:
    s=s.replace(old,new,1)
elif 'Audio reopen failed' not in s:
    raise SystemExit('rooted resumeAudio block changed')
runtime.write_text(s,encoding='utf-8')

s=controller.read_text(encoding='utf-8')
old='''    fun resumeAudio(): Boolean {\n        if (closed.get()) return false\n        if (!started.get()) return start()\n        val ok = runtime.resumeAudio()\n        if (!ok) statusChanged(engine.nativeLastTextError().ifBlank { "Audio resume failed" })\n        return ok\n    }\n'''
new='''    fun resumeAudio(): Boolean {\n        if (closed.get()) return false\n        if (!started.get()) return start()\n        return runtime.resumeAudio()\n    }\n'''
if old in s:
    s=s.replace(old,new,1)
elif 'return runtime.resumeAudio()' not in s:
    raise SystemExit('controller resumeAudio block changed')
controller.write_text(s,encoding='utf-8')
print('PASS: audio resume made idempotent and diagnostic-preserving')
