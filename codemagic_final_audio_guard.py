#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
root_mode = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'
runtime = root / 'android/app/src/main/java/com/silveton/sampler/SamplerRuntime.kt'
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'

for p in (root_mode, runtime, cpp):
    if not p.is_file():
        raise SystemExit(f'missing final audio source: {p}')

# Restore the proven runtime model:
#   enter() = one-time root/performance bootstrap while the Activity is alive.
#   refreshAudioPermissions() = the only operation repeated before PCM
#   open/resume/recovery. Re-running the whole bootstrap on every resume can
#   disturb an already working MediaTek audio state.
k = root_mode.read_text(encoding='utf-8')
enter0 = k.find('    fun enter(): Result = synchronized(lock) {')
if enter0 < 0:
    raise SystemExit('SilvetonRootRuntime.enter() missing')
uid0 = k.find('        val uid = Process.myUid()', enter0)
if uid0 < 0:
    raise SystemExit('enter() uid anchor missing')
enter_prefix = k[enter0:uid0]

# Remove comments inserted by the two experimental reassert patches.
for stale in [
    '        // Reassert live sampler grants/tuning even when already active.\n\n',
    '        // active state does not replace live ALSA permission refresh.\n\n',
]:
    enter_prefix = enter_prefix.replace(stale, '')

# Normalize to exactly one active short-circuit.
enter_prefix = enter_prefix.replace('        if (active) return@synchronized Result(true)\n\n', '')
enter_prefix += '        if (active) return@synchronized Result(true)\n\n'
k = k[:enter0] + enter_prefix + k[uid0:]
root_mode.write_text(k, encoding='utf-8')

# Validate the FINAL dedicated ALSA refresh gate.
k = root_mode.read_text(encoding='utf-8')
a0 = k.find('    fun refreshAudioPermissions(): Result {')
a1 = k.find('    fun refreshMidiPermissions(): Result {', a0)
if a0 < 0 or a1 < 0:
    raise SystemExit('final refreshAudioPermissions block missing')
audio = k[a0:a1]

required_audio = [
    'APPCTX=',
    'node_type()',
    'magiskpolicy',
    'allowxperm ${\'$\'}DOMAIN ${\'$\'}T chr_file ioctl 0x0000-0xffff',
    'chown ${uid}:1005',
    'chmod 0666',
    'ADDA_DL_CH1 DL1_CH1',
    'ADDA_DL_CH2 DL1_CH2',
    'DAC In Mux',
    'HPL Mux',
    'HPR Mux',
    'Headset Volume" 12 12',
]
for token in required_audio:
    if token not in audio:
        raise SystemExit(f'final Direct-ALSA audio contract missing: {token}')

# Physical/system protection remains intact. SELinux is only extended for this
# application domain and ALSA character devices; global enforcing is untouched.
for forbidden in [
    'setenforce 0',
    'setenforce permissive',
    '/sys/fs/selinux/enforce',
    '/sys/class/thermal',
    'thermal_zone',
    'speaker_protect',
    'Speaker Protect',
    'SmartPA',
    'smartpa',
]:
    if forbidden in k:
        raise SystemExit(f'unsafe global/physical protection override detected: {forbidden}')

# Proven SM-X130 PCM configuration.
s = cpp.read_text(encoding='utf-8')
for token in [
    'constexpr unsigned kRate = 48000U;',
    'constexpr unsigned kPeriods = 3U;',
    'kPreferredMinimumPeriodFrames = 96U;',
    'config.bufferFrames - config.periodFrames',
    'sw.xfer_align = 1U;',
]:
    if token not in s:
        raise SystemExit(f'final PCM stability contract missing: {token}')

# Root owns the mixer route. The ordinary APK must not require controlC0 in the
# openLocked PCM path.
open0 = s.find('    bool openLocked() {')
open1 = s.find('\n    bool startLocked() {', open0)
if open0 < 0 or open1 < 0:
    raise SystemExit('openLocked block missing')
open_block = s[open0:open1]
if 'configureOutputRouteLocked()' in open_block:
    raise SystemExit('ordinary APK still requires mixer control access in openLocked')
if 'chooseAndOpenPcmLocked()' not in open_block:
    raise SystemExit('Direct ALSA PCM open missing')

# Dedicated refresh must run before hardware use on every path, while enter()
# itself stays one-time.
r = runtime.read_text(encoding='utf-8')
start0 = r.find('    fun start(): Boolean {')
start1 = r.find('    fun suspendAudio()', start0)
resume0 = r.find('    fun resumeAudio(): Boolean {')
resume1 = r.find('    override fun close()', resume0)
if min(start0, start1, resume0, resume1) < 0:
    raise SystemExit('runtime start/resume method boundaries missing')
start_block = r[start0:start1]
resume_block = r[resume0:resume1]
if 'SilvetonRootRuntime.enter()' not in start_block or 'refreshAudioPermissions()' not in start_block or 'nativeAudioOpen()' not in start_block:
    raise SystemExit('start path lost root/ALSA sequence')
if start_block.find('refreshAudioPermissions()') > start_block.find('nativeAudioOpen()'):
    raise SystemExit('start path refresh happens after PCM open')
if 'SilvetonRootRuntime.enter()' not in resume_block or 'refreshAudioPermissions()' not in resume_block or 'nativeAudioStart()' not in resume_block:
    raise SystemExit('resume path lost root/ALSA sequence')

rec = r.find('private val recoveryPoll')
if rec >= 0:
    rec_end = r.find('    fun start(): Boolean {', rec)
    recovery = r[rec:rec_end if rec_end >= 0 else len(r)]
    if 'nativeAudioNeedsRecovery()' in recovery:
        if 'refreshAudioPermissions()' not in recovery:
            raise SystemExit('audio recovery path lost ALSA refresh')
        recover_call = recovery.find('nativeAudioRecover()')
        if recover_call >= 0 and recovery.find('refreshAudioPermissions()') > recover_call:
            raise SystemExit('recovery refresh happens after native recovery')

final_root = root_mode.read_text(encoding='utf-8')
enter0 = final_root.find('    fun enter(): Result = synchronized(lock) {')
uid0 = final_root.find('        val uid = Process.myUid()', enter0)
if final_root[enter0:uid0].count('if (active) return@synchronized Result(true)') != 1:
    raise SystemExit('root bootstrap is not exactly-once while active')

print('PASS: proven audio lifecycle restored — one-time root bootstrap + dedicated ALSA refresh; physical protections untouched')
