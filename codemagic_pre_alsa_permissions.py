from pathlib import Path
import sys

root = Path(sys.argv[1])
java_dir = root / 'android/app/src/main/java/com/silveton/sampler'
root_mode = java_dir / 'SilvetonRootRuntime.kt'
runtime = java_dir / 'SamplerRuntime.kt'
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'

for p in (root_mode, runtime, cpp):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

# The manual SM-X130 tests were done through a privileged shell. Reproducing only
# the node owner/mode inside an ordinary APK is not enough: the APK still runs in
# its own SELinux domain, and the mixer-control path can also reject writes before
# PCM is even opened. Make the root bootstrap own the mixer route and grant the
# exact APK domain access only to the PCM/raw-MIDI device nodes.
k = root_mode.read_text(encoding='utf-8')
start = k.find('    fun refreshAudioPermissions(): Result {')
end = k.find('    fun refreshMidiPermissions(): Result {', start)
if start < 0 or end < 0:
    raise SystemExit('refreshAudioPermissions/refreshMidiPermissions anchors missing')

refresh = r'''    fun refreshAudioPermissions(): Result {
        val uid = Process.myUid()
        val pid = Process.myPid()
        val script = """
            set -u
            CTL=/dev/snd/controlC0
            PCM=/dev/snd/pcmC0D0p

            [ -c "${'$'}CTL" ] || { echo "ALSA: missing ${'$'}CTL"; exit 51; }
            [ -c "${'$'}PCM" ] || { echo "ALSA: missing ${'$'}PCM"; exit 52; }

            APPCTX="${'$'}(cat /proc/''' + '${pid}' + r'''/attr/current 2>/dev/null || true)"
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

            # Apply each rule separately. This avoids one bad statement hiding
            # which rule failed and uses the live APK domain + live node type.
            grant_chr() {
                T="${'$'}1"
                "${'$'}MP" --live "allow ${'$'}DOMAIN ${'$'}T chr_file { getattr open read write append ioctl lock map }" >/dev/null 2>&1 || return 1
                "${'$'}MP" --live "allowxperm ${'$'}DOMAIN ${'$'}T chr_file ioctl 0x4100-0x41ff" >/dev/null 2>&1 || return 1
                "${'$'}MP" --live "allowxperm ${'$'}DOMAIN ${'$'}T chr_file ioctl 0x5500-0x55ff" >/dev/null 2>&1 || return 1
                "${'$'}MP" --live "allowxperm ${'$'}DOMAIN ${'$'}T chr_file ioctl 0x5700-0x57ff" >/dev/null 2>&1 || return 1
                return 0
            }

            "${'$'}MP" --live "allow ${'$'}DOMAIN device dir { getattr open read search }" >/dev/null 2>&1 || {
                echo "ALSA: SELinux /dev search grant failed for ${'$'}DOMAIN"; exit 55;
            }
            "${'$'}MP" --live "allow ${'$'}DOMAIN audio_device dir { getattr open read search }" >/dev/null 2>&1 || true
            grant_chr "${'$'}CTLTYPE" || { echo "ALSA: SELinux control grant failed ${'$'}DOMAIN -> ${'$'}CTLTYPE"; exit 56; }
            if [ "${'$'}PCMTYPE" != "${'$'}CTLTYPE" ]; then
                grant_chr "${'$'}PCMTYPE" || { echo "ALSA: SELinux PCM grant failed ${'$'}DOMAIN -> ${'$'}PCMTYPE"; exit 57; }
            fi

            # DAC permissions are made deterministic for the APK itself. The
            # app UID is the owner, so PCM access does not depend on membership
            # in Android's audio group. 0660 is sufficient and safer than 0666.
            for n in "${'$'}CTL" "${'$'}PCM" /dev/snd/midiC*D*; do
                [ -e "${'$'}n" ] || continue
                chown ''' + '${uid}' + r''':1005 "${'$'}n" 2>/dev/null || {
                    echo "ALSA: chown failed ${'$'}n"; exit 58;
                }
                chmod 0660 "${'$'}n" 2>/dev/null || {
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

'''
k = k[:start] + refresh + k[end:]

# Android /system/bin/sh + `set -u`: `$c_` is parsed as variable `c_`.
bad_idle_key = "key=\"idle_${'$'}c_${'$'}(basename"
good_idle_key = "key=\"idle_${'$'}{c}_${'$'}(basename"
if bad_idle_key in k:
    k = k.replace(bad_idle_key, good_idle_key)
elif good_idle_key not in k:
    raise SystemExit('CPU idle key shell anchor missing')

# The SM-X130 Magisk su build previously rejected the generated `su -c` form.
# Start an interactive root shell and feed the script through stdin instead.
old_proc = '''        val p = ProcessBuilder("su", "-c", script)\n            .redirectErrorStream(true)\n            .start()\n'''
new_proc = '''        val p = ProcessBuilder("su")\n            .redirectErrorStream(true)\n            .start()\n        p.outputStream.bufferedWriter().use { writer ->\n            writer.write(script)\n            writer.newLine()\n            writer.flush()\n        }\n'''
if old_proc in k:
    k = k.replace(old_proc, new_proc, 1)
elif 'ProcessBuilder("su")' in k and 'writer.write(script)' in k:
    pass
else:
    raise SystemExit('runSu process creation anchor missing')

root_mode.write_text(k, encoding='utf-8')

# Permission refresh must execute before every native ALSA open/reopen.
r = runtime.read_text(encoding='utf-8')
for method, next_method in [
    ('    fun start(): Boolean {', '    fun suspendAudio()'),
    ('    fun resumeAudio(): Boolean {', '    override fun close()'),
]:
    a = r.find(method)
    b = r.find(next_method, a)
    if a < 0 or b < 0:
        raise SystemExit(f'runtime method anchor missing: {method.strip()}')
    block = r[a:b]
    grant_pos = block.find('SilvetonRootRuntime.refreshAudioPermissions()')
    open_pos = block.find('engine.nativeAudioOpen()')
    if grant_pos < 0 or open_pos < 0 or grant_pos > open_pos:
        raise SystemExit(f'pre-ALSA permission order broken in {method.strip()}')

# The route is now guaranteed by the root gate. Do not make controlC0 access from
# the ordinary APK a second mandatory gate before PCM open. Keep the C++ mixer
# implementation in source as a diagnostic/fallback helper, but do not call it
# from openLocked(). This removes the first permission-denied boundary entirely.
s = cpp.read_text(encoding='utf-8')
open_start = s.find('    bool openLocked() {')
open_end = s.find('\n    bool startLocked() {', open_start)
if open_start < 0 or open_end < 0:
    raise SystemExit('C++ openLocked block missing')
block = s[open_start:open_end]
route_start = block.find('        if (!configureOutputRouteLocked()) {')
if route_start >= 0:
    pcm_start = block.find('        if (!chooseAndOpenPcmLocked()) {', route_start)
    if pcm_start < 0:
        raise SystemExit('PCM open block missing after mixer route')
    block = block[:route_start] + '''        // Mixer route is configured by the privileged pre-open root gate.\n        // The APK itself only owns the PCM realtime path.\n''' + block[pcm_start:]
    s = s[:open_start] + block + s[open_end:]
elif 'Mixer route is configured by the privileged pre-open root gate.' not in block:
    raise SystemExit('unexpected C++ mixer-route/openLocked structure')
cpp.write_text(s, encoding='utf-8')

# Final contracts: app UID DAC ownership, live-domain SELinux rule, privileged
# mixer route and no mandatory controlC0 access in the APK open path.
final = root_mode.read_text(encoding='utf-8')
for token in [
    'chown ${uid}:1005',
    'chmod 0660',
    'APPCTX=',
    'node_type()',
    'grant_chr()',
    'magiskpolicy',
    'ADDA_DL_CH1 DL1_CH1',
    'DAC In Mux',
    'Headset Volume',
    'ProcessBuilder("su")',
    'writer.write(script)',
    "key=\"idle_${'$'}{c}_${'$'}(basename",
]:
    if token not in final:
        raise SystemExit(f'deep ALSA/root contract missing: {token}')
if 'ProcessBuilder("su", "-c", script)' in final:
    raise SystemExit('stale su -c execution remains')

final_cpp = cpp.read_text(encoding='utf-8')
a = final_cpp.find('    bool openLocked() {')
b = final_cpp.find('\n    bool startLocked() {', a)
open_block = final_cpp[a:b]
if 'configureOutputRouteLocked()' in open_block:
    raise SystemExit('ordinary APK still requires controlC0 during openLocked')
if 'chooseAndOpenPcmLocked()' not in open_block:
    raise SystemExit('PCM open missing after privileged mixer split')

if "key=\"idle_${'$'}c_${'$'}(basename" in final:
    raise SystemExit('stale shell $c_ nounset bug remains')

print('PASS: Direct ALSA permission boundary split correctly (root mixer route + app-owned PCM + live SELinux domain)')
