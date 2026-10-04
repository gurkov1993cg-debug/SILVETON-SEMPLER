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
s = s.replace('96 frames x 2 at 96 kHz', '96 frames x 2 at 48 kHz')
s = s.replace('96 kHz / 96x2', '48 kHz / 96x2')

cpp.write_text(s, encoding='utf-8')

# Keep the source audit aligned with the actual runtime target. The audit must
# validate the real requested rate, not a stale 96 kHz literal.
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

# Apply the explicit root chmod/chown + SELinux grant immediately before the
# native ALSA open/reopen path. This stays out of the realtime audio thread.
perm = Path(__file__).with_name('codemagic_pre_alsa_permissions.py')
if not perm.is_file():
    raise SystemExit(f'missing pre-ALSA permission patch: {perm}')
exec(compile(perm.read_text(encoding='utf-8'), str(perm), 'exec'), {
    '__name__': '__main__',
    '__file__': str(perm),
})

print('PASS: SILVETON Direct ALSA target locked to 48 kHz / 96 frames x 2 periods + pre-open root grant')
