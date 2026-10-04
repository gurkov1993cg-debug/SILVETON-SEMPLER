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
 * The APK remains an ordinary APK, but uses the already-approved root service
 * to grant only the runtime access/tuning required by the sampler.
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

            DOMAIN="${'$'}(cat /proc/${pid}/attr/current 2>/dev/null | cut -d: -f3)"
            MP="${'$'}(command -v magiskpolicy 2>/dev/null || true)"
            [ -z "${'$'}MP" ] && [ -x /data/adb/magisk/magiskpolicy ] && MP=/data/adb/magisk/magiskpolicy
            if [ -z "${'$'}DOMAIN" ] || [ -z "${'$'}MP" ]; then
                echo "Magisk SELinux policy tool unavailable"
                exit 41
            fi

            # Narrow live SELinux grant for this app domain only. ALSA PCM,
            # control and raw-MIDI ioctl families use A/U/W ioctl type bytes.
            "${'$'}MP" --live \
                "allow ${'$'}DOMAIN audio_device dir { getattr open read search }" \
                "allow ${'$'}DOMAIN device dir { getattr open read search }" \
                "allow ${'$'}DOMAIN audio_device chr_file { getattr open read write ioctl lock map }" \
                "allowxperm ${'$'}DOMAIN audio_device chr_file ioctl 0x4100-0x41ff" \
                "allowxperm ${'$'}DOMAIN audio_device chr_file ioctl 0x5500-0x55ff" \
                "allowxperm ${'$'}DOMAIN audio_device chr_file ioctl 0x5700-0x57ff" \
                >/dev/null 2>&1 || {
                    echo "SELinux Direct-ALSA rule install failed"
                    exit 42
                }

            if [ ! -f "${'$'}STATE/active" ]; then
                save_node /dev/cpuset/background/cpus cpuset_background
                save_node /dev/cpuset/system-background/cpus cpuset_system_background
                save_node /dev/cpuset/restricted/cpus cpuset_restricted
                save_node /sys/devices/system/cpu/cpufreq/policy6/scaling_governor governor_policy6
                save_node /sys/devices/system/cpu/cpufreq/policy7/scaling_governor governor_policy7

                for c in 6 7; do
                    for st in /sys/devices/system/cpu/cpu${'$'}c/cpuidle/state*; do
                        [ -f "${'$'}st/disable" ] || continue
                        case "${'$'}st" in */state0) continue ;; esac
                        key="idle_${'$'}c_${'$'}(basename "${'$'}st")"
                        save_node "${'$'}st/disable" "${'$'}key"
                    done
                done

                AFE="${'$'}(grep -i 'afe_isr' /proc/interrupts 2>/dev/null | head -1 | awk -F: '{gsub(/ /,"",${'$'}1); print ${'$'}1}')"
                [ -n "${'$'}AFE" ] || AFE="${'$'}(grep -i 'afe' /proc/interrupts 2>/dev/null | head -1 | awk -F: '{gsub(/ /,"",${'$'}1); print ${'$'}1}')"
                if [ -n "${'$'}AFE" ] && [ -r "/proc/irq/${'$'}AFE/smp_affinity" ]; then
                    echo "${'$'}AFE" > "${'$'}STATE/afe_irq"
                    cat "/proc/irq/${'$'}AFE/smp_affinity" > "${'$'}STATE/afe_affinity"
                fi
                date +%s > "${'$'}STATE/active"
            fi

            # Linux DAC: only this SILVETON app UID + Android audio group.
            for n in /dev/snd/controlC0 /dev/snd/pcmC0D0p /dev/snd/midiC*D*; do
                [ -e "${'$'}n" ] || continue
                chown ${uid}:1005 "${'$'}n" 2>/dev/null || true
                chmod 0660 "${'$'}n" 2>/dev/null || true
            done

            # Keep background Android work off the two fast cores. Do not kill
            # services or cached apps: avoiding stop/start churn is more stable.
            write_node /dev/cpuset/background/cpus 0-5
            write_node /dev/cpuset/system-background/cpus 0-5
            write_node /dev/cpuset/restricted/cpus 0-5
            write_node /sys/devices/system/cpu/cpufreq/policy6/scaling_governor performance
            write_node /sys/devices/system/cpu/cpufreq/policy7/scaling_governor performance

            for c in 6 7; do
                for st in /sys/devices/system/cpu/cpu${'$'}c/cpuidle/state*; do
                    [ -f "${'$'}st/disable" ] || continue
                    case "${'$'}st" in */state0) continue ;; esac
                    echo 1 > "${'$'}st/disable" 2>/dev/null || true
                done
            done

            if [ -r "${'$'}STATE/afe_irq" ]; then
                AFE="${'$'}(cat "${'$'}STATE/afe_irq")"
                [ -w "/proc/irq/${'$'}AFE/smp_affinity" ] && echo 80 > "/proc/irq/${'$'}AFE/smp_affinity" 2>/dev/null || true
            fi

            renice -n -20 -p ${pid} >/dev/null 2>&1 || true
            exit 0
        """.trimIndent()

        val r = runSu(script, 15)
        if (r.ok) active = true
        r
    }

    /** Promote already-created native audio/MIDI threads after engine start. */
    fun promoteRealtimeThreads(): Result {
        val pid = Process.myPid()
        val script = """
            set -u
            promote() {
                NAME="${'$'}1"; MASK="${'$'}2"; PRIO="${'$'}3"
                for t in /proc/${pid}/task/[0-9]*; do
                    [ -r "${'$'}t/comm" ] || continue
                    [ "${'$'}(cat "${'$'}t/comm" 2>/dev/null)" = "${'$'}NAME" ] || continue
                    TID="${'$'}{t##*/}"
                    command -v taskset >/dev/null 2>&1 && taskset -p "${'$'}MASK" "${'$'}TID" >/dev/null 2>&1 || true
                    command -v chrt >/dev/null 2>&1 && chrt -f -p "${'$'}PRIO" "${'$'}TID" >/dev/null 2>&1 || true
                    renice -n -20 -p "${'$'}TID" >/dev/null 2>&1 || true
                done
            }
            promote SilvAudioRT 80 80
            promote SilvMidiRT 3f 70
            exit 0
        """.trimIndent()
        return runSu(script, 5)
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

            for c in 6 7; do
                for st in /sys/devices/system/cpu/cpu${'$'}c/cpuidle/state*; do
                    [ -f "${'$'}st/disable" ] || continue
                    case "${'$'}st" in */state0) continue ;; esac
                    key="idle_${'$'}c_${'$'}(basename "${'$'}st")"
                    restore_node "${'$'}st/disable" "${'$'}key"
                done
            done

            if [ -r "${'$'}STATE/afe_irq" ] && [ -r "${'$'}STATE/afe_affinity" ]; then
                AFE="${'$'}(cat "${'$'}STATE/afe_irq")"
                if [ -w "/proc/irq/${'$'}AFE/smp_affinity" ]; then
                    cat "${'$'}STATE/afe_affinity" > "/proc/irq/${'$'}AFE/smp_affinity" 2>/dev/null || true
                fi
            fi

            for n in /dev/snd/controlC0 /dev/snd/pcmC0D0p /dev/snd/midiC*D*; do
                [ -e "${'$'}n" ] || continue
                chown 1000:1005 "${'$'}n" 2>/dev/null || true
                chmod 0660 "${'$'}n" 2>/dev/null || true
            done

            rm -rf "${'$'}STATE"
            exit 0
        """.trimIndent()
        val r = runSu(script, 10)
        if (r.ok) active = false
        r
    }

    private fun runSu(script: String, timeoutSeconds: Long): Result = try {
        val p = ProcessBuilder("su", "-c", script)
            .redirectErrorStream(true)
            .start()
        val output = p.inputStream.bufferedReader().use { it.readText() }.trim()
        if (!p.waitFor(timeoutSeconds, TimeUnit.SECONDS)) {
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

    private fun promoteThreadsAsync() {
        Thread({
            try { Thread.sleep(80L) } catch (_: InterruptedException) { return@Thread }
            if (!closed) SilvetonRootRuntime.promoteRealtimeThreads()
        }, "SilvetonRtPromote").apply {
            priority = Thread.NORM_PRIORITY - 1
            start()
        }
    }

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
                        promoteThreadsAsync()
                        statusChanged(null)
                    }
                }
            }
            handler.postDelayed(this, 250L)
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
        promoteThreadsAsync()
        statusChanged(null)
        return true
    }

    fun suspendAudio() {
        if (closed) return
        // Keep the root performance envelope active while the sampler Activity
        // is alive. This avoids CPU/IRQ/governor churn on transient focus loss.
        engine.nativeAudioStop()
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
        if (!ok) {
            statusChanged(engine.nativeLastTextError())
        } else {
            promoteThreadsAsync()
            statusChanged(null)
        }
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
if '#include <pthread.h>' not in s:
    s = s.replace('#include <poll.h>\n', '#include <poll.h>\n#include <pthread.h>\n')

# Replace any older one-core helper produced by earlier patches.
start = s.find('static void pinCurrentThreadToCpu(')
if start >= 0:
    end = s.find('\n}\n\n', start)
    if end < 0:
        raise SystemExit('old affinity helper terminator not found')
    s = s[:start] + s[end + 4:]

anchor = 'constexpr const char* kTag = "SilvetonSampler";\n\n'
helper = r'''constexpr const char* kTag = "SilvetonSampler";

static void configureRealtimeThread(const char* name, int firstCpu, int lastCpu, int fifoPriority) noexcept {
    if (name != nullptr) (void)::pthread_setname_np(::pthread_self(), name);

    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = std::max(0, firstCpu); cpu <= std::max(firstCpu, lastCpu); ++cpu) CPU_SET(cpu, &set);
    (void)::sched_setaffinity(0, sizeof(set), &set);

    sched_param param{};
    param.sched_priority = fifoPriority;
    if (::sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        // Ordinary Android processes may not hold CAP_SYS_NICE. Root mode will
        // promote the named TID from outside; nice still gives a useful fallback.
        (void)::setpriority(PRIO_PROCESS, 0, -16);
    }
}

'''
if 'configureRealtimeThread(' not in s:
    if anchor not in s:
        raise SystemExit('C++ RT helper insertion point changed')
    s = s.replace(anchor, helper)

# Normalize older variants before applying the stability-first topology.
s = s.replace('        pinCurrentThreadToCpu(6);\n', '')
s = s.replace('        pinCurrentThreadToCpu(7);\n', '')
s = s.replace(
    '    void directMidiLoop() noexcept {\n        (void)::setpriority(PRIO_PROCESS, 0, -16);\n',
    '    void directMidiLoop() noexcept {\n        configureRealtimeThread("SilvMidiRT", 0, 5, 70);\n'
)
s = s.replace(
    '    void audioLoop() noexcept {\n        (void)::setpriority(PRIO_PROCESS, 0, -16);\n',
    '    void audioLoop() noexcept {\n        configureRealtimeThread("SilvAudioRT", 7, 7, 80);\n'
)

if 'configureRealtimeThread("SilvMidiRT", 0, 5, 70);' not in s:
    raise SystemExit('failed to install housekeeping-core Direct MIDI topology')
if 'configureRealtimeThread("SilvAudioRT", 7, 7, 80);' not in s:
    raise SystemExit('failed to install CPU7 audio topology')
if 'pinCurrentThreadToCpu(6);' in s:
    raise SystemExit('obsolete MIDI-on-CPU6 topology still present')
cpp.write_text(s, encoding='utf-8')

rt = runtime.read_text(encoding='utf-8')
rm = root_mode.read_text(encoding='utf-8')
checks = {
    'root runtime wired': 'SilvetonRootRuntime.enter()' in rt,
    'root kept during transient pause': 'SilvetonRootRuntime.leave()\n    }\n\n    fun resumeAudio' not in rt,
    'thread promotion': 'promoteRealtimeThreads()' in rt,
    'Magisk live policy': 'magiskpolicy' in rm,
    'PCM ioctl xperm': '0x4100-0x41ff' in rm,
    'control ioctl xperm': '0x5500-0x55ff' in rm,
    'raw MIDI ioctl xperm': '0x5700-0x57ff' in rm,
    'no cached-app kill': 'cmd activity kill-all' not in rm,
    'AFE IRQ CPU7': 'smp_affinity" ] && echo 80' in rm,
}
for name, ok in checks.items():
    if not ok:
        raise SystemExit(f'missing rooted APK stability contract: {name}')

print('PASS: rooted APK stability runtime installed (audio CPU7 / MIDI CPU0-5 / Direct ALSA / conservative isolation)')
