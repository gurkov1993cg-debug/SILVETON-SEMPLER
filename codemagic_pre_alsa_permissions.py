from pathlib import Path
import sys

root = Path(sys.argv[1])
java_dir = root / 'android/app/src/main/java/com/silveton/sampler'
root_mode = java_dir / 'SilvetonRootRuntime.kt'
runtime = java_dir / 'SamplerRuntime.kt'

for p in (root_mode, runtime):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

# Transitional rooted-APK permission gate. It runs immediately before the
# Direct-ALSA open and mirrors the SM-X130 node state proven manually to work.
k = root_mode.read_text(encoding='utf-8')
start = k.find('    fun refreshAudioPermissions(): Result {')
end = k.find('    fun refreshMidiPermissions(): Result {', start)
if start < 0 or end < 0:
    raise SystemExit('refreshAudioPermissions/refreshMidiPermissions anchors missing')

refresh = r'''    fun refreshAudioPermissions(): Result {
        val pid = Process.myPid()
        val script = """
            set -u
            CTL=/dev/snd/controlC0
            PCM=/dev/snd/pcmC0D0p

            [ -c "${'$'}CTL" ] || { echo "Missing ${'$'}CTL"; exit 51; }
            [ -c "${'$'}PCM" ] || { echo "Missing ${'$'}PCM"; exit 52; }

            DOMAIN="${'$'}(cat /proc/''' + '${pid}' + r'''/attr/current 2>/dev/null | cut -d: -f3)"
            MP="${'$'}(command -v magiskpolicy 2>/dev/null || true)"
            [ -z "${'$'}MP" ] && [ -x /data/adb/magisk/magiskpolicy ] && MP=/data/adb/magisk/magiskpolicy
            if [ -z "${'$'}DOMAIN" ] || [ -z "${'$'}MP" ]; then
                echo "ALSA grant: Magisk SELinux policy tool unavailable"
                exit 53
            fi

            "${'$'}MP" --live \
                "allow ${'$'}DOMAIN audio_device dir { getattr open read search }" \
                "allow ${'$'}DOMAIN device dir { getattr open read search }" \
                "allow ${'$'}DOMAIN audio_device chr_file { getattr open read write ioctl lock map }" \
                "allowxperm ${'$'}DOMAIN audio_device chr_file ioctl 0x4100-0x41ff" \
                "allowxperm ${'$'}DOMAIN audio_device chr_file ioctl 0x5500-0x55ff" \
                "allowxperm ${'$'}DOMAIN audio_device chr_file ioctl 0x5700-0x57ff" \
                >/dev/null 2>&1 || {
                    echo "ALSA grant: SELinux policy install failed"
                    exit 54
                }

            # Match the state that was physically proven on this SM-X130:
            # owner media (1013), group audio (1005), rw for all. SELinux still
            # remains enforcing, so the app-domain magiskpolicy rule above is
            # still required.
            for n in "${'$'}CTL" "${'$'}PCM" /dev/snd/midiC*D*; do
                [ -e "${'$'}n" ] || continue
                chown 1013:1005 "${'$'}n" 2>/dev/null || {
                    echo "ALSA grant: chown failed for ${'$'}n"
                    exit 55
                }
                chmod 0666 "${'$'}n" 2>/dev/null || {
                    echo "ALSA grant: chmod failed for ${'$'}n"
                    exit 56
                }
            done

            exit 0
        """.trimIndent()
        return runSu(script, 5)
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

# Avoid `su -c` on this device: open su and feed the command script on stdin.
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

final = root_mode.read_text(encoding='utf-8')
for token in [
    'chown 1013:1005',
    'chmod 0666',
    '/dev/snd/midiC*D*',
    'pcmC0D0p',
    'controlC0',
    'magiskpolicy',
    'ProcessBuilder("su")',
    'writer.write(script)',
    "key=\"idle_${'$'}{c}_${'$'}(basename",
]:
    if token not in final:
        raise SystemExit(f'pre-ALSA/root contract missing: {token}')
if 'ProcessBuilder("su", "-c", script)' in final:
    raise SystemExit('stale su -c execution remains')
if 'chmod 0660 "${'$'}n"' in final:
    raise SystemExit('stale restrictive ALSA chmod remains in refreshed gate')
if "key=\"idle_${'$'}c_${'$'}(basename" in final:
    raise SystemExit('stale shell $c_ nounset bug remains')

print('PASS: SM-X130 pre-ALSA gate matches proven media:audio / 0666 node state')
