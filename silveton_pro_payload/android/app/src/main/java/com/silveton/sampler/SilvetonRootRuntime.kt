package com.silveton.sampler

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
            cmd appops set com.silveton.sampler MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1 || true
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

            # SILVETON takes the sound card away from Android while it runs.
            if [ ! -s "${'$'}STATE/audio_services" ]; then
                getprop | grep -E '^\[init\.svc\..*audio.*\]: \[running\]' \
                    | sed 's/^\[init\.svc\.\(.*\)\]: .*/\1/' > "${'$'}STATE/audio_services" 2>/dev/null || true
            fi
            while read -r svc; do
                [ -n "${'$'}svc" ] && stop "${'$'}svc" >/dev/null 2>&1 || true
            done < "${'$'}STATE/audio_services"
            sleep 0.3

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
                        key="idle_${'$'}{c}_${'$'}(basename "${'$'}st")"
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
            for t in /proc/${pid}/task/[0-9]*; do
                [ -r "${'$'}t/comm" ] || continue
                NAME="${'$'}(cat "${'$'}t/comm" 2>/dev/null)"
                case "${'$'}NAME" in
                    SilvAudioRT|SilvMidiRT) continue ;;
                esac
                TID="${'$'}{t##*/}"
                command -v taskset >/dev/null 2>&1 && taskset -p 3f "${'$'}TID" >/dev/null 2>&1 || true
            done
            promote SilvAudioRT 80 85
            promote SilvMidiRT 40 75
            exit 0
        """.trimIndent()
        return runSu(script, 5)
    }

    fun refreshBankStorageAccess(): Result {
        val script = """
            set -u
            cmd appops set com.silveton.sampler MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1 || {
                echo "Could not grant all-files access"
                exit 61
            }
            exit 0
        """.trimIndent()
        return runSu(script, 5)
    }

    fun refreshAudioPermissions(): Result {
        val uid = Process.myUid()
        val pid = Process.myPid()
        val script = """
            set -u
            CTL=/dev/snd/controlC0
            PCM=/dev/snd/pcmC0D0p

            [ -c "${'$'}CTL" ] || { echo "ALSA: missing ${'$'}CTL"; exit 51; }
            [ -c "${'$'}PCM" ] || { echo "ALSA: missing ${'$'}PCM"; exit 52; }

            APPCTX="${'$'}(cat /proc/${pid}/attr/current 2>/dev/null || true)"
            DOMAIN="${'$'}(printf '%s' "${'$'}APPCTX" | cut -d: -f3)"
            [ -n "${'$'}DOMAIN" ] || { echo "ALSA: cannot resolve APK SELinux domain (${ '$' }APPCTX)"; exit 53; }

            MP="${'$'}(command -v magiskpolicy 2>/dev/null || true)"
            [ -z "${'$'}MP" ] && [ -x /data/adb/magisk/magiskpolicy ] && MP=/data/adb/magisk/magiskpolicy
            [ -n "${'$'}MP" ] || { echo "ALSA: magiskpolicy unavailable"; exit 54; }

            # Read the real target type from the live device node instead of
            # assuming that every firmware labels it exactly audio_device.
            node_type() {
                ls -Z "${'$'}1" 2>/dev/null | awk '{
                    for (i=1; i<=NF; ++i) {
                        if (${'$'}i ~ /^u:object_r:/) {
                            n=split(${'$'}i,a,":");
                            if (n >= 3) { print a[3]; exit }
                        }
                    }
                }'
            }
            CTLTYPE="${'$'}(node_type "${'$'}CTL")"
            PCMTYPE="${'$'}(node_type "${'$'}PCM")"
            [ -n "${'$'}CTLTYPE" ] || CTLTYPE=audio_device
            [ -n "${'$'}PCMTYPE" ] || PCMTYPE=audio_device

            # SILVETON Direct-ALSA: remove ALSA SELinux restrictions for
            # this live app domain and these live ALSA character-device types.
            grant_alsa_unrestricted() {
                T="${'$'}1"
                "${'$'}MP" --live "allow ${'$'}DOMAIN ${'$'}T chr_file { getattr open read write append ioctl lock map }" >/dev/null 2>&1 || return 1
                "${'$'}MP" --live "allowxperm ${'$'}DOMAIN ${'$'}T chr_file ioctl 0x0000-0xffff" >/dev/null 2>&1 || return 1
                return 0
            }

            "${'$'}MP" --live "allow ${'$'}DOMAIN device dir { getattr open read search }" >/dev/null 2>&1 || {
                echo "ALSA: /dev SELinux access removal failed for ${'$'}DOMAIN"; exit 55;
            }
            "${'$'}MP" --live "allow ${'$'}DOMAIN audio_device dir { getattr open read search }" >/dev/null 2>&1 || true
            grant_alsa_unrestricted "${'$'}CTLTYPE" || {
                echo "ALSA: control SELinux restrictions could not be removed ${'$'}DOMAIN -> ${'$'}CTLTYPE"; exit 56;
            }
            if [ "${'$'}PCMTYPE" != "${'$'}CTLTYPE" ]; then
                grant_alsa_unrestricted "${'$'}PCMTYPE" || {
                    echo "ALSA: PCM SELinux restrictions could not be removed ${'$'}DOMAIN -> ${'$'}PCMTYPE"; exit 57;
                }
            fi

            # DAC permissions are made deterministic for the APK itself. The
            # app UID is the owner, so PCM access does not depend on membership
            # in Android's audio group. 0660 is sufficient and safer than 0666.
            for n in "${'$'}CTL" "${'$'}PCM" /dev/snd/midiC*D*; do
                [ -e "${'$'}n" ] || continue
                chown ${uid}:1005 "${'$'}n" 2>/dev/null || {
                    echo "ALSA: chown failed ${'$'}n"; exit 58;
                }
                chmod 0666 "${'$'}n" 2>/dev/null || {
                    echo "ALSA: chmod failed ${'$'}n"; exit 59;
                }
            done

            # The mixer route is privileged work. Do it here as root instead of
            # making the untrusted APK open controlC0 and write MediaTek controls.
            TM="${'$'}(command -v tinymix 2>/dev/null || true)"
            [ -z "${'$'}TM" ] && [ -x /vendor/bin/tinymix ] && TM=/vendor/bin/tinymix
            [ -z "${'$'}TM" ] && [ -x /system/bin/tinymix ] && TM=/system/bin/tinymix
            [ -n "${'$'}TM" ] || { echo "ALSA: tinymix unavailable for root mixer route"; exit 60; }

            "${'$'}TM" "ADDA_DL_CH1 DL1_CH1" 1 >/dev/null 2>&1 || { echo "ALSA: route CH1 failed"; exit 61; }
            "${'$'}TM" "ADDA_DL_CH2 DL1_CH2" 1 >/dev/null 2>&1 || { echo "ALSA: route CH2 failed"; exit 62; }
            "${'$'}TM" "DAC In Mux" "Normal Path" >/dev/null 2>&1 || { echo "ALSA: DAC mux failed"; exit 63; }
            "${'$'}TM" "HPL Mux" "Audio Playback" >/dev/null 2>&1 || { echo "ALSA: HPL mux failed"; exit 64; }
            "${'$'}TM" "HPR Mux" "Audio Playback" >/dev/null 2>&1 || { echo "ALSA: HPR mux failed"; exit 65; }
            "${'$'}TM" "Headset Volume" 12 12 >/dev/null 2>&1 || { echo "ALSA: headset volume failed"; exit 66; }

            echo "ALSA_READY domain=${'$'}DOMAIN ctl=${'$'}CTLTYPE pcm=${'$'}PCMTYPE"
            exit 0
        """.trimIndent()
        return runSu(script, 8)
    }

    fun refreshMidiPermissions(): Result {
        val uid = Process.myUid()
        val script = """
            for n in /dev/snd/midiC*D*; do
                [ -e "${'$'}n" ] || continue
                chown ${uid}:1005 "${'$'}n" 2>/dev/null || true
                chmod 0660 "${'$'}n" 2>/dev/null || true
            done
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
                    key="idle_${'$'}{c}_${'$'}(basename "${'$'}st")"
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

            if [ -r "${'$'}STATE/audio_services" ]; then
                while read -r svc; do
                    [ -n "${'$'}svc" ] && start "${'$'}svc" >/dev/null 2>&1 || true
                done < "${'$'}STATE/audio_services"
            fi

            rm -rf "${'$'}STATE"
            exit 0
        """.trimIndent()
        val r = runSu(script, 10)
        if (r.ok) active = false
        r
    }

    private fun runSu(script: String, timeoutSeconds: Long): Result = try {
        val p = ProcessBuilder("su")
            .redirectErrorStream(true)
            .start()
        p.outputStream.bufferedWriter().use { writer ->
            writer.write(script)
            writer.newLine()
            writer.flush()
        }

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

}
