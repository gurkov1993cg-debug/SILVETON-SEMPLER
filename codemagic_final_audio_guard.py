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

# This is the final audio gate. It is intentionally narrow in scope:
# - keep global SELinux ENFORCING;
# - keep Android/kernel thermal and speaker-protection mechanisms untouched;
# - never write unknown SmartPA/speaker-protection mixer controls;
# - only reassert the already-proven Direct-ALSA device access and mixer route.
k = root_mode.read_text(encoding='utf-8')

# Android may recreate /dev/snd nodes while the process remains alive. Never let
# the cached in-process active flag skip the live refresh path.
k = k.replace('        if (active) return@synchronized Result(true)\n\n',
              '        // active state does not replace live ALSA permission refresh.\n\n', 1)
root_mode.write_text(k, encoding='utf-8')

# Validate the FINAL refreshAudioPermissions implementation after every previous
# Codemagic patch has run.
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

# Do not globally weaken the OS or defeat physical protection systems. We only
# grant this app domain access to the live ALSA character devices.
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

# The PCM runtime itself must remain the measured stable SM-X130 configuration.
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

# The privileged root gate owns the mixer route. The ordinary APK must not make
# controlC0 a second mandatory permission boundary before opening PCM.
open0 = s.find('    bool openLocked() {')
open1 = s.find('\n    bool startLocked() {', open0)
if open0 < 0 or open1 < 0:
    raise SystemExit('openLocked block missing')
open_block = s[open0:open1]
if 'configureOutputRouteLocked()' in open_block:
    raise SystemExit('ordinary APK still requires mixer control access in openLocked')
if 'chooseAndOpenPcmLocked()' not in open_block:
    raise SystemExit('Direct ALSA PCM open missing')

# Every hardware start/restart path must refresh access immediately before using
# ALSA. This is what prevents Android from silently re-applying an EACCES barrier.
r = runtime.read_text(encoding='utf-8')
if 'SilvetonRootRuntime.refreshAudioPermissions()' not in r:
    raise SystemExit('runtime no longer refreshes Direct-ALSA permissions')

start0 = r.find('    fun start(): Boolean {')
start1 = r.find('    fun suspendAudio()', start0)
resume0 = r.find('    fun resumeAudio(): Boolean {')
resume1 = r.find('    override fun close()', resume0)
if min(start0, start1, resume0, resume1) < 0:
    raise SystemExit('runtime start/resume method boundaries missing')
start_block = r[start0:start1]
resume_block = r[resume0:resume1]
if 'refreshAudioPermissions()' not in start_block or 'nativeAudioOpen()' not in start_block:
    raise SystemExit('start path lost pre-open ALSA refresh')
if start_block.find('refreshAudioPermissions()') > start_block.find('nativeAudioOpen()'):
    raise SystemExit('start path refresh happens after PCM open')
if 'refreshAudioPermissions()' not in resume_block or 'nativeAudioStart()' not in resume_block:
    raise SystemExit('resume path lost ALSA refresh')

# Recovery is allowed to differ structurally, but it must refresh access before
# native recovery/restart if that path exists.
rec = r.find('private val recoveryPoll')
if rec >= 0:
    rec_end = r.find('    fun start(): Boolean {', rec)
    recovery = r[rec:rec_end if rec_end >= 0 else len(r)]
    if 'nativeAudioNeedsRecovery()' in recovery:
        if 'refreshAudioPermissions()' not in recovery:
            raise SystemExit('audio recovery path lost ALSA permission refresh')
        recover_call = recovery.find('nativeAudioRecover()')
        if recover_call >= 0 and recovery.find('refreshAudioPermissions()') > recover_call:
            raise SystemExit('recovery refresh happens after native recovery')

print('PASS: final audio guard — Direct ALSA access reasserted; SELinux/thermal/speaker protection left intact')
