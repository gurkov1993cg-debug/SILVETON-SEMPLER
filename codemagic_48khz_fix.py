from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
audit = root / 'ci/audit_direct_alsa.py'

if not cpp.is_file():
    raise SystemExit(f'missing Direct ALSA bridge: {cpp}')

s = cpp.read_text(encoding='utf-8')

# SILVETON sampler target rate: 48 kHz. Keep the low-latency 96-frame / 2-period
# Direct-ALSA topology; only the engine/device sample rate changes here.
if 'constexpr unsigned kRate = 96000U;' in s:
    s = s.replace('constexpr unsigned kRate = 96000U;', 'constexpr unsigned kRate = 48000U;', 1)
elif 'constexpr unsigned kRate = 48000U;' not in s:
    raise SystemExit('Direct ALSA sample-rate constant not found')

s = s.replace('96 kHz / 96 x 2', '48 kHz / 96 x 2')
s = s.replace('96 frames x 2 at 96 kHz', '48 kHz / 96 x 2')
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

# Build the rooted pre-open gate. It resolves the live APK SELinux domain and the
# live PCM/control node labels, grants open/read/write/ioctl/lock/map, and grants
# the complete ALSA PCM/control/raw-MIDI ioctl families before nativeAudioOpen().
perm = Path(__file__).with_name('codemagic_pre_alsa_permissions.py')
if not perm.is_file():
    raise SystemExit(f'missing pre-ALSA permission patch: {perm}')
exec(compile(perm.read_text(encoding='utf-8'), str(perm), 'exec'), {
    '__name__': '__main__',
    '__file__': str(perm),
})

# During hardware validation remove Unix DAC as a possible source of EACCES.
# SELinux stays enforcing; access is still limited by the live-domain rules above.
root_mode = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'
if not root_mode.is_file():
    raise SystemExit(f'missing root runtime: {root_mode}')
k = root_mode.read_text(encoding='utf-8')
a0 = k.find('    fun refreshAudioPermissions(): Result {')
a1 = k.find('    fun refreshMidiPermissions(): Result {', a0)
if a0 < 0 or a1 < 0:
    raise SystemExit('refreshAudioPermissions block missing')
block = k[a0:a1]
block = block.replace('chmod 0660 "${\'$\'}n"', 'chmod 0666 "${\'$\'}n"')
k = k[:a0] + block + k[a1:]
root_mode.write_text(k, encoding='utf-8')

# Refuse to build if any of the known PCM permission boundaries are still absent.
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
    'allow ${\'$\'}DOMAIN ${\'$\'}T chr_file { getattr open read write append ioctl lock map }',
    '0x4100-0x41ff',
    '0x5500-0x55ff',
    '0x5700-0x57ff',
]:
    if token not in audio_block:
        raise SystemExit(f'PCM permission contract missing: {token}')
if 'chmod 0660' in audio_block:
    raise SystemExit('restrictive PCM DAC mode remains in refreshAudioPermissions')

# Android /system/bin/sh + `set -u`: disambiguate `$c_` into `${c}_`.
shellfix = Path(__file__).with_name('codemagic_shell_nounset_fix.py')
if not shellfix.is_file():
    raise SystemExit(f'missing Android shell nounset patch: {shellfix}')
exec(compile(shellfix.read_text(encoding='utf-8'), str(shellfix), 'exec'), {
    '__name__': '__main__',
    '__file__': str(shellfix),
})

print('PASS: SILVETON Direct ALSA 48 kHz / 96x2 + PCM DAC open + live-domain SELinux ALSA grants')
