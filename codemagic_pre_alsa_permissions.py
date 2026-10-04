from pathlib import Path
import sys

root = Path(sys.argv[1])
java_dir = root / 'android/app/src/main/java/com/silveton/sampler'
root_mode = java_dir / 'SilvetonRootRuntime.kt'
runtime = java_dir / 'SamplerRuntime.kt'

for p in (root_mode, runtime):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

# Replace the audio permission refresh with an explicit pre-ALSA root gate.
# It runs before nativeAudioOpen(), never from the RT audio thread.
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

            [ -c "${'$'}CTL" ] || { echo "Missing ${'$'}CTL"; exit 51; }
            [ -c "${'$'}PCM" ] || { echo "Missing ${'$'}PCM"; exit 52; }

            DOMAIN="${'$'}(cat /proc/''' + '${pid}' + r'''/attr/current 2>/dev/null | cut -d: -f3)"
            MP="${'$'}(command -v magiskpolicy 2>/dev/null || true)"
            [ -z "${'$'}MP" ] && [ -x /data/adb/magisk/magiskpolicy ] && MP=/data/adb/magisk/magiskpolicy
            if [ -z "${'$'}DOMAIN" ] || [ -z "${'$'}MP" ]; then
                echo "ALSA grant: Magisk SELinux policy tool unavailable"
                exit 53
            fi

            # Re-apply the app-domain Direct-ALSA rule before every open/reopen.
            # This is intentionally outside the realtime thread.
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

            # Give only this SILVETON app UID + Android audio group access.
            # Equivalent purpose to chmod/chown via su, but avoids world-writable 0666.
            for n in "${'$'}CTL" "${'$'}PCM"; do
                chown ''' + '${uid}' + r''':1005 "${'$'}n" 2>/dev/null || {
                    echo "ALSA grant: chown failed for ${'$'}n"
                    exit 55
                }
                chmod 0660 "${'$'}n" 2>/dev/null || {
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
root_mode.write_text(k, encoding='utf-8')

# Prove source order: permission grant must happen before native ALSA open.
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
    'chown ${uid}:1005',
    'chmod 0660',
    'pcmC0D0p',
    'controlC0',
    'magiskpolicy',
    '0x4100-0x41ff',
]:
    if token not in final:
        raise SystemExit(f'pre-ALSA permission contract missing: {token}')

print('PASS: pre-ALSA root permission gate installed before every PCM open/reopen')
