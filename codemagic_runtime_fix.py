from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
controller = root / 'android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt'
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'

for p in (cpp, controller, view):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

s = cpp.read_text(encoding='utf-8')
old = '''        setIntParam(params, SNDRV_PCM_HW_PARAM_RATE, requestedRate);\n        setMinParam(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, kPreferredMinimumPeriodFrames);\n        setIntParam(params, SNDRV_PCM_HW_PARAM_PERIODS, requestedPeriods);\n'''
new = '''        setIntParam(params, SNDRV_PCM_HW_PARAM_RATE, requestedRate);\n        // SM-X130 has already been measured clean at exactly 96 frames x 2.\n        // Keep the hardware contract exact: no hidden larger period is allowed.\n        setIntParam(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, kPreferredMinimumPeriodFrames);\n        setIntParam(params, SNDRV_PCM_HW_PARAM_PERIODS, requestedPeriods);\n'''
if old not in s and new not in s:
    raise SystemExit('unexpected HW_PARAMS block')
s = s.replace(old, new)

old = '''        if (config.rate != requestedRate || config.periodFrames == 0U || config.bufferFrames < config.periodFrames) {\n            errorOut = -EINVAL;\n            return false;\n        }\n'''
new = '''        if (config.rate != requestedRate\n            || config.periodFrames != kPreferredMinimumPeriodFrames\n            || config.periods != requestedPeriods\n            || config.bufferFrames != config.periodFrames * config.periods) {\n            errorOut = -EINVAL;\n            return false;\n        }\n'''
if old not in s and new not in s:
    raise SystemExit('unexpected PCM validation block')
s = s.replace(old, new)

start = s.find('    bool chooseAndOpenPcmLocked() {')
end = s.find('\n    bool openLocked() {', start)
if start < 0 or end < 0:
    raise SystemExit('chooseAndOpenPcmLocked not found')
replacement = '''    bool chooseAndOpenPcmLocked() {\n        // Physical SM-X130 tests established the target path at 96 kHz,\n        // 96 frames/period and two periods. Do not probe 48 kHz or silently\n        // enlarge the queue: that makes startup slower and hides regressions.\n        constexpr unsigned kRate = 96000U;\n        constexpr unsigned kPeriods = 2U;\n\n        const int fd = openPcmNode();\n        if (fd < 0) {\n            const int error = -errno;\n            lastError_.store(error, std::memory_order_relaxed);\n            return false;\n        }\n\n        PcmConfig selected{};\n        int error = 0;\n        if (!configurePcmFd(fd, kRate, kPeriods, selected, error)) {\n            releasePcmFd(fd);\n            lastError_.store(error, std::memory_order_relaxed);\n            return false;\n        }\n\n        pcmFd_.store(fd, std::memory_order_release);\n        sampleRate_.store(static_cast<int>(selected.rate), std::memory_order_relaxed);\n        framesPerBurst_.store(static_cast<int>(selected.periodFrames), std::memory_order_relaxed);\n        bufferSizeFrames_.store(static_cast<int>(selected.bufferFrames), std::memory_order_relaxed);\n        periodCount_.store(static_cast<int>(selected.periods), std::memory_order_relaxed);\n\n        const std::size_t sampleCount = static_cast<std::size_t>(selected.periodFrames) * kChannels;\n        renderBuffer_.assign(sampleCount, 0.0F);\n        pcmBuffer_.assign(sampleCount, 0);\n\n        __android_log_print(\n            ANDROID_LOG_INFO, kTag,\n            "Direct ALSA locked %s: %u Hz, period=%u, periods=%u, buffer=%u (%.3f ms)",\n            kPcmDevice, selected.rate, selected.periodFrames, selected.periods,\n            selected.bufferFrames, queueCapacityMs(selected));\n        return true;\n    }\n'''
s = s[:start] + replacement + s[end:]

old = '''        if (!chooseAndOpenPcmLocked()) {\n            const int error = lastError_.load(std::memory_order_relaxed);\n            setTextError(\n                std::string("Direct ALSA open/config failed for ") + kPcmDevice\n                + " (errno " + std::to_string(-error) + ")");\n            return false;\n        }\n'''
new = '''        if (!chooseAndOpenPcmLocked()) {\n            const int error = lastError_.load(std::memory_order_relaxed);\n            if (error == -EACCES || error == -EPERM) {\n                setTextError(\n                    std::string("Direct ALSA permission denied for ") + kPcmDevice\n                    + ". The SM-X130 stock SELinux policy blocks ordinary APK access to audio_device.");\n            } else {\n                setTextError(\n                    std::string("Direct ALSA open/config failed for ") + kPcmDevice\n                    + " (errno " + std::to_string(-error) + ")");\n            }\n            return false;\n        }\n'''
if old not in s and new not in s:
    raise SystemExit('unexpected openLocked error block')
s = s.replace(old, new)
cpp.write_text(s, encoding='utf-8')

s = controller.read_text(encoding='utf-8')
old = '''        loadDefaultBankOnce()\n        if (!runtime.start()) return false\n        midi.start()\n        started.set(true)\n        return true\n'''
new = '''        if (!runtime.start()) return false\n        midi.start()\n        started.set(true)\n        // Bank parsing/disk I/O must never block Activity.onStart()/the UI thread.\n        Thread({ loadDefaultBankOnce() }, "SilvetonBankLoad").apply {\n            priority = Thread.NORM_PRIORITY - 1\n            start()\n        }\n        return true\n'''
if old not in s and new not in s:
    raise SystemExit('unexpected SamplerAppController.start block')
s = s.replace(old, new)
controller.write_text(s, encoding='utf-8')

s = view.read_text(encoding='utf-8')
old = '''    private var globalState: SamplerAppController.GlobalUiState? = null\n'''
new = '''    private var globalState: SamplerAppController.GlobalUiState? = null\n    private var frameAudioStatus: SamplerAppController.AudioStatus = sampler.audioStatus()\n    private var frameUpperMeters: List<Pair<Float, Float>> = List(7) { 0f to 0f }\n'''
if old not in s and new not in s:
    raise SystemExit('view state insertion point changed')
s = s.replace(old, new)
old = '''        hits.clear()\n        canvas.drawColor(C_BG)\n'''
new = '''        hits.clear()\n        // Cache native telemetry once for this frame. Drawing code below must not\n        // issue repeated JNI calls or PCM ioctls from the UI thread.\n        frameAudioStatus = sampler.audioStatus()\n        frameUpperMeters = sampler.upperMeters()\n        canvas.drawColor(C_BG)\n'''
if old not in s and new not in s:
    raise SystemExit('onDraw insertion point changed')
s = s.replace(old, new)
s = s.replace('        postInvalidateDelayed(40L)\n', '        postInvalidateDelayed(50L)\n')
s = s.replace('        val status = sampler.audioStatus()\n', '        val status = frameAudioStatus\n')
s = s.replace('        val st = sampler.audioStatus()\n', '        val st = frameAudioStatus\n')
s = s.replace('        val meters = sampler.upperMeters()\n', '        val meters = frameUpperMeters\n')
view.write_text(s, encoding='utf-8')

final_cpp = cpp.read_text(encoding='utf-8')
checks = {
    '96k exact': 'constexpr unsigned kRate = 96000U;',
    '2 periods exact': 'constexpr unsigned kPeriods = 2U;',
    '96 frame exact': 'SNDRV_PCM_HW_PARAM_PERIOD_SIZE, kPreferredMinimumPeriodFrames',
    'no 48k probing': 'kRates{{96000U, 48000U}}',
    'permission diagnostic': 'stock SELinux policy blocks ordinary APK access',
}
for name, needle in checks.items():
    present = needle in final_cpp
    if name == 'no 48k probing':
        if present:
            raise SystemExit('48 kHz probing still present')
    elif not present:
        raise SystemExit(f'missing runtime fix: {name}')
print('PASS: Codemagic runtime patch applied (Direct ALSA 96k/96x2 + UI startup/telemetry fixes)')
