from pathlib import Path
import sys

root = Path(sys.argv[1])
java_dir = root / 'android/app/src/main/java/com/silveton/sampler'
runtime = java_dir / 'SamplerRuntime.kt'
root_mode = java_dir / 'SilvetonRootRuntime.kt'
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'

for p in (runtime, cpp):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

root_mode.write_text(r'''package com.silveton.sampler

import android.os.Process
import java.util.concurrent.TimeUnit

/**
 * Root bootstrap for the dedicated SM-X130.
 * One APK owns the full flow: Direct ALSA permissions + low-load live mode.
 * No separate Magisk module or system-integration ZIP is required.
 */
internal object SilvetonRootRuntime {
    data class Result(val ok: Boolean, val message: String = "")

    private val lock = Any()
    @Volatile private var active = false

    fun enter(): Result = synchronized(lock) {
        if (active) return@synchronized Result(true)

        val uid = Process.myUid()
        val pid = Process.myPid()
        val script = """
            set -u
            STATE=/data/local/tmp/silveton-root-state
            mkdir -p "${'$'}STATE"
            chmod 0700 "${'$'}STATE"

            save_node() {
                p="${'$'}1"; n="${'$'}2"
                [ -r "${'$'}p" ] && [ ! -f "${'$'}STATE/${'$'}n" ] && cat "${'$'}p" > "${'$'}STATE/${'$'}n"
            }
            write_node() {
                p="${'$'}1"; v="${'$'}2"
                [ -w "${'$'}p" ] && echo "${'$'}v" > "${'$'}p" 2>/dev/null || true
            }

            if [ ! -f "${'$'}STATE/active" ]; then
                save_node /dev/cpuset/background/cpus cpuset_background
                save_node /dev/cpuset/system-background/cpus cpuset_system_background
                save_node /dev/cpuset/restricted/cpus cpuset_restricted
                save_node /sys/devices/system/cpu/cpufreq/policy6/scaling_governor governor_policy6
                save_node /sys/devices/system/cpu/cpufreq/policy7/scaling_governor governor_policy7
                date +%s > "${'$'}STATE/active"
            fi

            # Linux DAC: only this SILVETON app UID + Android audio group.
            for n in /dev/snd/controlC0 /dev/snd/pcmC0D0p /dev/snd/midiC*D*; do
                [ -e "${'$'}n" ] || continue
                chown ${uid}:1005 "${'$'}n" 2>/dev/null || true
                chmod 0660 "${'$'}n" 2>/dev/null || true
            done

            # SELinux: patch only the actual app domain. DAC still restricts nodes to this UID.
            DOMAIN="${'$'}(cat /proc/${pid}/attr/current 2>/dev/null | cut -d: -f3)"
            MP="${'$'}(command -v magiskpolicy 2>/dev/null || true)"
            [ -z "${'$'}MP" ] && [ -x /data/adb/magisk/magiskpolicy ] && MP=/data/adb/magisk/magiskpolicy
            if [ -n "${'$'}MP" ] && [ -n "${'$'}DOMAIN" ]; then
                "${'$'}MP" --live "allow ${'$'}DOMAIN audio_device dir { getattr open read search }" >/dev/null 2>&1 || true
                "${'$'}MP" --live "allow ${'$'}DOMAIN audio_device chr_file { getattr open read write ioctl lock map }" >/dev/null 2>&1 || true
                "${'$'}MP" --live "allowxperm ${'$'}DOMAIN audio_device chr_file ioctl 0x4100-0x41ff" >/dev/null 2>&1 || true
            fi

            # Keep non-audio Android work away from the two fast cores used by SILVETON.
            write_node /dev/cpuset/background/cpus 0-5
            write_node /dev/cpuset/system-background/cpus 0-5
            write_node /dev/cpuset/restricted/cpus 0-5
            write_node /sys/devices/system/cpu/cpufreq/policy6/scaling_governor performance
            write_node /sys/devices/system/cpu/cpufreq/policy7/scaling_governor performance

            # Boost this process and clear cached user apps once. Core Android services are not stopped.
            renice -n -20 -p ${pid} >/dev/null 2>&1 || true
            /system/bin/cmd activity kill-all >/dev/null 2>&1 || true

            # Hard verification: Direct ALSA nodes must be accessible to this UID after setup.
            [ -r /dev/snd/controlC0 ] || exit 31
            [ -w /dev/snd/controlC0 ] || exit 32
            [ -r /dev/snd/pcmC0D0p ] || exit 33
            [ -w /dev/snd/pcmC0D0p ] || exit 34
            exit 0
        """.trimIndent()

        val r = runSu(script)
        if (r.ok) active = true
        r
    }

    fun leave(): Result = synchronized(lock) {
        if (!active) return@synchronized Result(true)
        val script = """
            set -u
            STATE=/data/local/tmp/silveton-root-state
            restore_node() {
                p="${'$'}1"; n="${'$'}2"
                if [ -w "${'$'}p" ] && [ -r "${'$'}STATE/${'$'}n" ]; then
                    cat "${'$'}STATE/${'$'}n" > "${'$'}p" 2>/dev/null || true
                fi
            }
            restore_node /dev/cpuset/background/cpus cpuset_background
            restore_node /dev/cpuset/system-background/cpus cpuset_system_background
            restore_node /dev/cpuset/restricted/cpus cpuset_restricted
            restore_node /sys/devices/system/cpu/cpufreq/policy6/scaling_governor governor_policy6
            restore_node /sys/devices/system/cpu/cpufreq/policy7/scaling_governor governor_policy7

            for n in /dev/snd/controlC0 /dev/snd/pcmC0D0p /dev/snd/midiC*D*; do
                [ -e "${'$'}n" ] || continue
                chown 1000:1005 "${'$'}n" 2>/dev/null || true
                chmod 0660 "${'$'}n" 2>/dev/null || true
            done

            rm -rf "${'$'}STATE"
            exit 0
        """.trimIndent()
        val r = runSu(script)
        if (r.ok) active = false
        r
    }

    private fun runSu(script: String): Result = try {
        val p = ProcessBuilder("su", "-c", script)
            .redirectErrorStream(true)
            .start()
        val output = p.inputStream.bufferedReader().use { it.readText() }.trim()
        if (!p.waitFor(15, TimeUnit.SECONDS)) {
            p.destroyForcibly()
            Result(false, "Root setup timed out")
        } else if (p.exitValue() == 0) {
            Result(true)
        } else {
            Result(false, "Root setup failed (${p.exitValue()}): $output")
        }
    } catch (t: Throwable) {
        Result(false, "Root unavailable: ${t.message ?: t.javaClass.simpleName}")
    }
}
''', encoding='utf-8')

runtime.write_text(r'''package com.silveton.sampler

import android.os.Handler
import android.os.Looper

/** Lifecycle owner for the rooted Direct-ALSA sampler stream. */
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
                val root = SilvetonRootRuntime.enter()
                if (!root.ok) {
                    statusChanged(root.message)
                } else {
                    val error = engine.nativeLastAudioError()
                    if (!engine.nativeAudioRecover()) {
                        statusChanged("Audio recovery failed: $error ${engine.nativeLastTextError()}")
                    } else if (!engine.nativeAudioStart()) {
                        statusChanged(engine.nativeLastTextError().ifBlank { "Audio restart failed (${engine.nativeLastAudioError()})" })
                    } else {
                        statusChanged(null)
                    }
                }
            }
            handler.postDelayed(this, 500L)
        }
    }

    fun start(): Boolean {
        if (closed) return false
        val root = SilvetonRootRuntime.enter()
        if (!root.ok) {
            statusChanged(root.message)
            return false
        }
        if (!running) {
            running = true
            handler.post(recoveryPoll)
        }
        if (!engine.nativeAudioOpen()) {
            statusChanged(engine.nativeLastTextError().ifBlank { "Audio open failed (${engine.nativeLastAudioError()})" })
            SilvetonRootRuntime.leave()
            return false
        }
        if (!engine.nativeAudioStart()) {
            statusChanged(engine.nativeLastTextError().ifBlank { "Audio start failed (${engine.nativeLastAudioError()})" })
            SilvetonRootRuntime.leave()
            return false
        }
        statusChanged(null)
        return true
    }

    fun suspendAudio() {
        if (closed) return
        engine.nativeAudioStop()
        SilvetonRootRuntime.leave()
    }

    fun resumeAudio(): Boolean {
        if (closed) return false
        val root = SilvetonRootRuntime.enter()
        if (!root.ok) {
            statusChanged(root.message)
            return false
        }
        if (engine.nativeAudioNeedsRecovery() && !engine.nativeAudioRecover()) {
            statusChanged(engine.nativeLastTextError())
            return false
        }
        val ok = engine.nativeAudioStart()
        if (!ok) statusChanged(engine.nativeLastTextError()) else statusChanged(null)
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
        SilvetonRootRuntime.leave()
    }
}
''', encoding='utf-8')

s = cpp.read_text(encoding='utf-8')
if '#include <sched.h>' not in s:
    s = s.replace('#include <sys/resource.h>\n', '#include <sys/resource.h>\n#include <sched.h>\n')

anchor = 'constexpr const char* kTag = "SilvetonSampler";\n\n'
helper = '''constexpr const char* kTag = "SilvetonSampler";\n\nstatic void pinCurrentThreadToCpu(int cpu) noexcept {\n    cpu_set_t set;\n    CPU_ZERO(&set);\n    CPU_SET(cpu, &set);\n    (void)::sched_setaffinity(0, sizeof(set), &set);\n}\n\n'''
if 'pinCurrentThreadToCpu' not in s:
    if anchor not in s:
        raise SystemExit('C++ affinity insertion point changed')
    s = s.replace(anchor, helper)

s = s.replace(
    '    void directMidiLoop() noexcept {\n        (void)::setpriority(PRIO_PROCESS, 0, -16);\n',
    '    void directMidiLoop() noexcept {\n        pinCurrentThreadToCpu(6);\n        (void)::setpriority(PRIO_PROCESS, 0, -16);\n'
)
s = s.replace(
    '    void audioLoop() noexcept {\n        (void)::setpriority(PRIO_PROCESS, 0, -16);\n',
    '    void audioLoop() noexcept {\n        pinCurrentThreadToCpu(7);\n        (void)::setpriority(PRIO_PROCESS, 0, -16);\n'
)

if 'pinCurrentThreadToCpu(6);' not in s or 'pinCurrentThreadToCpu(7);' not in s:
    raise SystemExit('failed to install CPU affinity for Direct MIDI/audio')
cpp.write_text(s, encoding='utf-8')

if 'SilvetonRootRuntime.enter()' not in runtime.read_text(encoding='utf-8'):
    raise SystemExit('root runtime not wired')
if 'magiskpolicy' not in root_mode.read_text(encoding='utf-8'):
    raise SystemExit('SELinux live policy bootstrap missing')
if 'chown ${uid}:1005' not in root_mode.read_text(encoding='utf-8'):
    raise SystemExit('per-app ALSA ownership bootstrap missing')

print('PASS: single rooted APK runtime installed (Direct ALSA + live performance mode)')
