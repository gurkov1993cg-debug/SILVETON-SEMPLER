from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
audit = root / 'ci/audit_direct_alsa.py'

if not cpp.is_file():
    raise SystemExit(f'missing Direct ALSA bridge: {cpp}')

s = cpp.read_text(encoding='utf-8')

# SILVETON sampler target rate: 48 kHz / 96 frames / 2 periods.
if 'constexpr unsigned kRate = 96000U;' in s:
    s = s.replace('constexpr unsigned kRate = 96000U;', 'constexpr unsigned kRate = 48000U;', 1)
elif 'constexpr unsigned kRate = 48000U;' not in s:
    raise SystemExit('Direct ALSA sample-rate constant not found')

s = s.replace('96 kHz / 96 x 2', '48 kHz / 96 x 2')
s = s.replace('96 frames x 2 at 96 kHz', '96 frames x 2 at 48 kHz')
s = s.replace('96 kHz / 96x2', '48 kHz / 96x2')
cpp.write_text(s, encoding='utf-8')

if audit.is_file():
    a = audit.read_text(encoding='utf-8')
    a = a.replace('96000U', '48000U')
    a = a.replace('96000', '48000')
    a = a.replace('96 kHz', '48 kHz')
    audit.write_text(a, encoding='utf-8')

final = cpp.read_text(encoding='utf-8')
if 'constexpr unsigned kRate = 48000U;' not in final:
    raise SystemExit('48 kHz Direct ALSA contract missing')
if 'constexpr unsigned kRate = 96000U;' in final:
    raise SystemExit('stale 96 kHz Direct ALSA target remains')
if 'kPreferredMinimumPeriodFrames = 96U;' not in final:
    raise SystemExit('96-frame low-latency period contract missing')
if 'constexpr unsigned kPeriods = 2U;' not in final:
    raise SystemExit('2-period low-latency contract missing')

# Generate the root/audio permission gate first.
perm = Path(__file__).with_name('codemagic_pre_alsa_permissions.py')
if not perm.is_file():
    raise SystemExit(f'missing pre-ALSA permission patch: {perm}')
exec(compile(perm.read_text(encoding='utf-8'), str(perm), 'exec'), {
    '__name__': '__main__',
    '__file__': str(perm),
})

root_mode = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'
if not root_mode.is_file():
    raise SystemExit(f'missing root runtime: {root_mode}')
k = root_mode.read_text(encoding='utf-8')

a0 = k.find('    fun refreshAudioPermissions(): Result {')
a1 = k.find('    fun refreshMidiPermissions(): Result {', a0)
if a0 < 0 or a1 < 0:
    raise SystemExit('refreshAudioPermissions block missing')
block = k[a0:a1]

# Remove the previous narrow ALSA SELinux allow/xperm gate completely. For the
# SILVETON audio node we allow the full ioctl number space so SELinux cannot be
# the source of a PCM/control permission denial. SELinux remains globally
# enforcing; this change is scoped to the live app domain -> live ALSA node type.
sel_start = block.find('            # Apply each rule separately.')
sel_end = block.find('            # DAC permissions', sel_start)
if sel_start < 0 or sel_end < 0:
    raise SystemExit('narrow ALSA SELinux section missing')

broad_selinux = r'''            # SILVETON Direct-ALSA: remove ALSA SELinux restrictions for
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

'''
block = block[:sel_start] + broad_selinux + block[sel_end:]

# Also remove Unix DAC as a possible EACCES source during hardware validation.
block = block.replace('chmod 0660 "${\'$\'}n"', 'chmod 0666 "${\'$\'}n"')

k = k[:a0] + block + k[a1:]
root_mode.write_text(k, encoding='utf-8')

# Hard build contract: there must be no narrow ioctl ranges left in the audio
# refresh gate. The ALSA path must have full 0x0000-0xffff ioctl access.
final_root = root_mode.read_text(encoding='utf-8')
b0 = final_root.find('    fun refreshAudioPermissions(): Result {')
b1 = final_root.find('    fun refreshMidiPermissions(): Result {', b0)
audio_block = final_root[b0:b1]
for token in [
    'chmod 0666',
    'chown ${uid}:1005',
    'APPCTX=',
    'PCMTYPE=',
    'magiskpolicy',
    'grant_alsa_unrestricted()',
    'allowxperm ${\'$\'}DOMAIN ${\'$\'}T chr_file ioctl 0x0000-0xffff',
]:
    if token not in audio_block:
        raise SystemExit(f'unrestricted ALSA permission contract missing: {token}')
for stale in ['0x4100-0x41ff', '0x5500-0x55ff', '0x5700-0x57ff', 'grant_chr()']:
    if stale in audio_block:
        raise SystemExit(f'stale narrow ALSA SELinux restriction remains: {stale}')
if 'chmod 0660' in audio_block:
    raise SystemExit('restrictive PCM DAC mode remains in refreshAudioPermissions')

shellfix = Path(__file__).with_name('codemagic_shell_nounset_fix.py')
if not shellfix.is_file():
    raise SystemExit(f'missing Android shell nounset patch: {shellfix}')
exec(compile(shellfix.read_text(encoding='utf-8'), str(shellfix), 'exec'), {
    '__name__': '__main__',
    '__file__': str(shellfix),
})

print('PASS: SILVETON Direct ALSA 48 kHz / 96x2 + ALSA SELinux ioctl restrictions removed + DAC 0666')
