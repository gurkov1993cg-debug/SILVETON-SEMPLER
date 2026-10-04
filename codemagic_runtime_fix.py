from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
runtime = root / 'android/app/src/main/java/com/silveton/sampler/SamplerRuntime.kt'
controller = root / 'android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt'
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
activity = root / 'android/app/src/main/java/com/silveton/sampler/MainActivity.kt'
manifest = root / 'android/app/src/main/AndroidManifest.xml'

for p in (cpp, runtime, controller, view, activity, manifest):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

s = cpp.read_text(encoding='utf-8')

old = '''    static constexpr const char* kPcmDevice = "/dev/snd/pcmC0D0p";\n    static constexpr const char* kAlsaDeviceDirectory = "/dev/snd";\n    static constexpr unsigned kChannels = 2U;\n    static constexpr unsigned kPreferredMinimumPeriodFrames = 96U;\n'''
new = '''    static constexpr const char* kPcmDevice = "/dev/snd/pcmC0D0p";\n    static constexpr const char* kControlDevice = "/dev/snd/controlC0";\n    static constexpr const char* kAlsaDeviceDirectory = "/dev/snd";\n    static constexpr unsigned kChannels = 2U;\n    static constexpr unsigned kPreferredMinimumPeriodFrames = 96U;\n'''
if old not in s and new not in s:
    raise SystemExit('unexpected Direct ALSA constants block')
s = s.replace(old, new)

route_anchor = '''    static int openPcmNode() noexcept {\n        return ::open(kPcmDevice, O_RDWR | O_CLOEXEC);\n    }\n\n    static void releasePcmFd(int fd) noexcept {\n'''
route_code = '''    static std::optional<snd_ctl_elem_id> findControlId(int fd, const char* name) noexcept {\n        if (fd < 0 || name == nullptr) return std::nullopt;\n        snd_ctl_elem_list list{};\n        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0 || list.count == 0U) return std::nullopt;\n\n        std::vector<snd_ctl_elem_id> ids(list.count);\n        list.offset = 0U;\n        list.space = list.count;\n        list.pids = ids.data();\n        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) return std::nullopt;\n\n        const unsigned used = std::min<unsigned>(list.used, static_cast<unsigned>(ids.size()));\n        for (unsigned i = 0U; i < used; ++i) {\n            const char* controlName = reinterpret_cast<const char*>(ids[i].name);\n            if (std::strncmp(controlName, name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN) == 0) return ids[i];\n        }\n        return std::nullopt;\n    }\n\n    static bool setIntegerControl(int fd, const char* name, long first, long second, int& errorOut) noexcept {\n        const auto id = findControlId(fd, name);\n        if (!id.has_value()) { errorOut = -ENOENT; return false; }\n\n        snd_ctl_elem_info info{};\n        info.id = *id;\n        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { errorOut = -errno; return false; }\n        if (info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN && info.type != SNDRV_CTL_ELEM_TYPE_INTEGER) {\n            errorOut = -EINVAL;\n            return false;\n        }\n\n        snd_ctl_elem_value value{};\n        value.id = *id;\n        const unsigned count = std::min<unsigned>(info.count, 128U);\n        for (unsigned i = 0U; i < count; ++i) value.value.integer.value[i] = (i == 0U) ? first : second;\n        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) < 0) { errorOut = -errno; return false; }\n        errorOut = 0;\n        return true;\n    }\n\n    static bool setEnumControl(int fd, const char* name, const char* itemName, int& errorOut) noexcept {\n        const auto id = findControlId(fd, name);\n        if (!id.has_value()) { errorOut = -ENOENT; return false; }\n\n        snd_ctl_elem_info info{};\n        info.id = *id;\n        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { errorOut = -errno; return false; }\n        if (info.type != SNDRV_CTL_ELEM_TYPE_ENUMERATED || info.value.enumerated.items == 0U) {\n            errorOut = -EINVAL;\n            return false;\n        }\n\n        unsigned selected = info.value.enumerated.items;\n        for (unsigned item = 0U; item < info.value.enumerated.items; ++item) {\n            snd_ctl_elem_info itemInfo{};\n            itemInfo.id = *id;\n            itemInfo.value.enumerated.item = item;\n            if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &itemInfo) < 0) { errorOut = -errno; return false; }\n            if (std::strncmp(itemInfo.value.enumerated.name, itemName, sizeof(itemInfo.value.enumerated.name)) == 0) {\n                selected = item;\n                break;\n            }\n        }\n        if (selected >= info.value.enumerated.items) { errorOut = -ENOENT; return false; }\n\n        snd_ctl_elem_value value{};\n        value.id = *id;\n        const unsigned count = std::min<unsigned>(info.count, 128U);\n        for (unsigned i = 0U; i < count; ++i) value.value.enumerated.item[i] = selected;\n        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) < 0) { errorOut = -errno; return false; }\n        errorOut = 0;\n        return true;\n    }\n\n    bool configureOutputRouteLocked() {\n        const int fd = ::open(kControlDevice, O_RDWR | O_CLOEXEC);\n        if (fd < 0) {\n            lastError_.store(-errno, std::memory_order_relaxed);\n            return false;\n        }\n\n        int error = 0;\n        const bool ok = setIntegerControl(fd, "ADDA_DL_CH1 DL1_CH1", 1L, 1L, error)\n            && setIntegerControl(fd, "ADDA_DL_CH2 DL1_CH2", 1L, 1L, error)\n            && setEnumControl(fd, "DAC In Mux", "Normal Path", error)\n            && setEnumControl(fd, "HPL Mux", "Audio Playback", error)\n            && setEnumControl(fd, "HPR Mux", "Audio Playback", error);\n\n        if (ok) {\n            int volumeError = 0;\n            (void)setIntegerControl(fd, "Headset Volume", 12L, 12L, volumeError);\n        }\n\n        const int closeResult = ::close(fd);\n        if (!ok) {\n            lastError_.store(error == 0 ? -EIO : error, std::memory_order_relaxed);\n            return false;\n        }\n        if (closeResult < 0) {\n            lastError_.store(-errno, std::memory_order_relaxed);\n            return false;\n        }\n\n        __android_log_print(ANDROID_LOG_INFO, kTag, "Direct ALSA SM-X130 output route configured");\n        return true;\n    }\n\n    static int openPcmNode() noexcept {\n        return ::open(kPcmDevice, O_RDWR | O_CLOEXEC);\n    }\n\n    static void releasePcmFd(int fd) noexcept {\n'''
if 'configureOutputRouteLocked()' not in s:
    if route_anchor not in s:
        raise SystemExit('openPcmNode insertion point changed')
    s = s.replace(route_anchor, route_code)

old = '''        setIntParam(params, SNDRV_PCM_HW_PARAM_RATE, requestedRate);\n        setMinParam(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, kPreferredMinimumPeriodFrames);\n        setIntParam(params, SNDRV_PCM_HW_PARAM_PERIODS, requestedPeriods);\n'''
new = '''        setIntParam(params, SNDRV_PCM_HW_PARAM_RATE, requestedRate);\n        // Physical SM-X130 validation proved exactly 96 frames x 2 at 96 kHz.\n        setIntParam(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, kPreferredMinimumPeriodFrames);\n        setIntParam(params, SNDRV_PCM_HW_PARAM_PERIODS, requestedPeriods);\n'''
if old in s:
    s = s.replace(old, new)
elif new not in s:
    raise SystemExit('unexpected HW_PARAMS block')

old = '''        if (config.rate != requestedRate || config.periodFrames == 0U || config.bufferFrames < config.periodFrames) {\n            errorOut = -EINVAL;\n            return false;\n        }\n'''
new = '''        if (config.rate != requestedRate\n            || config.periodFrames != kPreferredMinimumPeriodFrames\n            || config.periods != requestedPeriods\n            || config.bufferFrames != config.periodFrames * config.periods) {\n            errorOut = -EINVAL;\n            return false;\n        }\n'''
if old in s:
    s = s.replace(old, new)
elif new not in s:
    raise SystemExit('unexpected PCM validation block')

s = s.replace(
    '        sw.xfer_align = static_cast<snd_pcm_uframes_t>(std::max(1U, config.periodFrames / 2U));\n',
    '        sw.xfer_align = 1U;\n'
)

start = s.find('    bool chooseAndOpenPcmLocked() {')
end = s.find('\n    bool openLocked() {', start)
if start < 0 or end < 0:
    raise SystemExit('chooseAndOpenPcmLocked not found')
replacement = '''    bool chooseAndOpenPcmLocked() {\n        // Direct ALSA bypasses Android AudioPolicy. Lock the exact hardware\n        // profile already proven on the physical SM-X130: 96 kHz / 96 x 2.\n        constexpr unsigned kRate = 96000U;\n        constexpr unsigned kPeriods = 2U;\n\n        const int fd = openPcmNode();\n        if (fd < 0) {\n            const int error = -errno;\n            lastError_.store(error, std::memory_order_relaxed);\n            if (error == -EACCES || error == -EPERM) {\n                recoverRequested_.store(true, std::memory_order_release);\n            }\n            return false;\n        }\n\n        PcmConfig selected{};\n        int error = 0;\n        if (!configurePcmFd(fd, kRate, kPeriods, selected, error)) {\n            releasePcmFd(fd);\n            lastError_.store(error, std::memory_order_relaxed);\n            return false;\n        }\n\n        pcmFd_.store(fd, std::memory_order_release);\n        sampleRate_.store(static_cast<int>(selected.rate), std::memory_order_relaxed);\n        framesPerBurst_.store(static_cast<int>(selected.periodFrames), std::memory_order_relaxed);\n        bufferSizeFrames_.store(static_cast<int>(selected.bufferFrames), std::memory_order_relaxed);\n        periodCount_.store(static_cast<int>(selected.periods), std::memory_order_relaxed);\n\n        const std::size_t sampleCount = static_cast<std::size_t>(selected.periodFrames) * kChannels;\n        renderBuffer_.assign(sampleCount, 0.0F);\n        pcmBuffer_.assign(sampleCount, 0);\n\n        __android_log_print(\n            ANDROID_LOG_INFO, kTag,\n            "Direct ALSA locked %s: %u Hz, period=%u, periods=%u, buffer=%u (%.3f ms)",\n            kPcmDevice, selected.rate, selected.periodFrames, selected.periods,\n            selected.bufferFrames, queueCapacityMs(selected));\n        return true;\n    }\n'''
s = s[:start] + replacement + s[end:]

open_start = s.find('    bool openLocked() {')
open_end = s.find('\n    bool startLocked() {', open_start)
if open_start < 0 or open_end < 0:
    raise SystemExit('openLocked block not found')
old_open = s[open_start:open_end]
marker = '        const int sr = sampleRate();\n'
tail_pos = old_open.find(marker)
if tail_pos < 0:
    raise SystemExit('openLocked prepare marker missing')
tail = old_open[tail_pos:]
new_open = '''    bool openLocked() {\n        if (pcmFd_.load(std::memory_order_acquire) >= 0) return true;\n        if (!configureOutputRouteLocked()) {\n            const int error = lastError_.load(std::memory_order_relaxed);\n            if (error == -EACCES || error == -EPERM) {\n                recoverRequested_.store(true, std::memory_order_release);\n                setTextError("Direct ALSA permission denied for /dev/snd. Install the SILVETON SM-X130 audio privilege module.");\n            } else {\n                setTextError("Direct ALSA mixer route failed (errno " + std::to_string(-error) + ")");\n            }\n            return false;\n        }\n        if (!chooseAndOpenPcmLocked()) {\n            const int error = lastError_.load(std::memory_order_relaxed);\n            if (error == -EACCES || error == -EPERM) {\n                recoverRequested_.store(true, std::memory_order_release);\n                setTextError("Direct ALSA permission denied for /dev/snd. Install the SILVETON SM-X130 audio privilege module.");\n            } else {\n                setTextError(\n                    std::string("Direct ALSA open/config failed for ") + kPcmDevice\n                    + " (errno " + std::to_string(-error) + ")");\n            }\n            return false;\n        }\n\n''' + tail
s = s[:open_start] + new_open + s[open_end:]
cpp.write_text(s, encoding='utf-8')

s = runtime.read_text(encoding='utf-8')
old = '''    private val recoveryPoll = object : Runnable {\n        override fun run() {\n            if (closed) return\n            if (engine.nativeAudioNeedsRecovery()) {\n                val error = engine.nativeLastAudioError()\n                if (!engine.nativeAudioRecover()) {\n                    statusChanged("Audio recovery failed: $error ${engine.nativeLastTextError()}")\n                } else {\n                    statusChanged(null)\n                }\n            }\n            handler.postDelayed(this, 500L)\n        }\n    }\n\n    fun start(): Boolean {\n        if (closed) return false\n        if (!engine.nativeAudioOpen()) {\n            statusChanged(engine.nativeLastTextError().ifBlank { "Audio open failed (${engine.nativeLastAudioError()})" })\n            return false\n        }\n        if (!engine.nativeAudioStart()) {\n            statusChanged(engine.nativeLastTextError().ifBlank { "Audio start failed (${engine.nativeLastAudioError()})" })\n            return false\n        }\n        if (!running) {\n            running = true\n            handler.post(recoveryPoll)\n        }\n        statusChanged(null)\n        return true\n    }\n'''
new = '''    private val recoveryPoll = object : Runnable {\n        override fun run() {\n            if (closed) return\n            if (engine.nativeAudioNeedsRecovery()) {\n                val error = engine.nativeLastAudioError()\n                if (!engine.nativeAudioRecover()) {\n                    statusChanged("Audio recovery failed: $error ${engine.nativeLastTextError()}")\n                } else if (!engine.nativeAudioStart()) {\n                    statusChanged(engine.nativeLastTextError().ifBlank { "Audio restart failed (${engine.nativeLastAudioError()})" })\n                } else {\n                    statusChanged(null)\n                }\n            }\n            handler.postDelayed(this, 500L)\n        }\n    }\n\n    fun start(): Boolean {\n        if (closed) return false\n        if (!running) {\n            running = true\n            handler.post(recoveryPoll)\n        }\n        if (!engine.nativeAudioOpen()) {\n            statusChanged(engine.nativeLastTextError().ifBlank { "Audio open failed (${engine.nativeLastAudioError()})" })\n            return false\n        }\n        if (!engine.nativeAudioStart()) {\n            statusChanged(engine.nativeLastTextError().ifBlank { "Audio start failed (${engine.nativeLastAudioError()})" })\n            return false\n        }\n        statusChanged(null)\n        return true\n    }\n'''
if old in s:
    s = s.replace(old, new)
elif new not in s:
    raise SystemExit('unexpected SamplerRuntime recovery/start block')
runtime.write_text(s, encoding='utf-8')

s = controller.read_text(encoding='utf-8')
old = '''        loadDefaultBankOnce()\n        if (!runtime.start()) return false\n        midi.start()\n        started.set(true)\n        return true\n'''
new = '''        if (!runtime.start()) return false\n        midi.start()\n        started.set(true)\n        Thread({ loadDefaultBankOnce() }, "SilvetonBankLoad").apply {\n            priority = Thread.NORM_PRIORITY - 1\n            start()\n        }\n        return true\n'''
if old in s:
    s = s.replace(old, new)
elif new not in s:
    raise SystemExit('unexpected SamplerAppController.start block')
controller.write_text(s, encoding='utf-8')

s = view.read_text(encoding='utf-8')
old = '    private var globalState: SamplerAppController.GlobalUiState? = null\n'
new = '''    private var globalState: SamplerAppController.GlobalUiState? = null\n    private var frameAudioStatus: SamplerAppController.AudioStatus = sampler.audioStatus()\n    private var frameUpperMeters: List<Pair<Float, Float>> = List(7) { 0f to 0f }\n'''
if old in s:
    s = s.replace(old, new)
elif new not in s:
    raise SystemExit('view state insertion point changed')
old = '''        hits.clear()\n        canvas.drawColor(C_BG)\n'''
new = '''        hits.clear()\n        frameAudioStatus = sampler.audioStatus()\n        frameUpperMeters = sampler.upperMeters()\n        canvas.drawColor(C_BG)\n'''
if old in s:
    s = s.replace(old, new)
elif new not in s:
    raise SystemExit('onDraw insertion point changed')
s = s.replace('        postInvalidateDelayed(40L)\n', '        postInvalidateDelayed(50L)\n')
s = s.replace('        val status = sampler.audioStatus()\n', '        val status = frameAudioStatus\n')
s = s.replace('        val st = sampler.audioStatus()\n', '        val st = frameAudioStatus\n')
s = s.replace('        val meters = sampler.upperMeters()\n', '        val meters = frameUpperMeters\n')
view.write_text(s, encoding='utf-8')

activity.write_text('''package com.silveton.sampler\n\nimport android.content.pm.ActivityInfo\nimport android.os.Bundle\nimport androidx.appcompat.app.AppCompatActivity\nimport androidx.core.view.WindowCompat\nimport androidx.core.view.WindowInsetsCompat\nimport androidx.core.view.WindowInsetsControllerCompat\nimport java.util.concurrent.Executors\n\n/** Android lifecycle host for the functional SILVETON SAMPLER touchscreen UI. */\nclass MainActivity : AppCompatActivity() {\n    private lateinit var sampler: SamplerAppController\n    private lateinit var samplerView: SilvetonSamplerView\n    private val audioLifecycle = Executors.newSingleThreadExecutor { runnable ->\n        Thread(runnable, "SilvetonAudioLifecycle").apply { priority = Thread.NORM_PRIORITY + 1 }\n    }\n    @Volatile private var created = false\n\n    override fun onCreate(savedInstanceState: Bundle?) {\n        super.onCreate(savedInstanceState)\n        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE\n        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)\n        hideSystemUi()\n\n        sampler = SamplerAppController(\n            this,\n            statusChanged = { warning ->\n                if (::samplerView.isInitialized) runOnUiThread { if (created) samplerView.setWarning(warning) }\n            },\n            midiConnectionChanged = { count ->\n                if (::samplerView.isInitialized) runOnUiThread { if (created) samplerView.setMidiConnections(count) }\n            }\n        )\n        samplerView = SilvetonSamplerView(this, sampler)\n        setContentView(samplerView)\n        created = true\n    }\n\n    override fun onStart() {\n        super.onStart()\n        if (!created) return\n        audioLifecycle.execute {\n            val ok = sampler.start()\n            if (!ok) {\n                val message = sampler.engine.nativeLastTextError().ifBlank { "Sampler start failed" }\n                runOnUiThread { if (created) samplerView.setWarning(message) }\n            }\n        }\n    }\n\n    override fun onResume() {\n        super.onResume()\n        hideSystemUi()\n        if (!created) return\n        audioLifecycle.execute {\n            if (!sampler.resumeAudio()) {\n                val message = sampler.engine.nativeLastTextError().ifBlank { "Audio resume failed" }\n                runOnUiThread { if (created) samplerView.setWarning(message) }\n            }\n        }\n    }\n\n    override fun onPause() {\n        if (created) audioLifecycle.execute { sampler.pauseAudio() }\n        super.onPause()\n    }\n\n    override fun onDestroy() {\n        created = false\n        audioLifecycle.execute { sampler.close() }\n        audioLifecycle.shutdown()\n        super.onDestroy()\n    }\n\n    override fun onWindowFocusChanged(hasFocus: Boolean) {\n        super.onWindowFocusChanged(hasFocus)\n        if (hasFocus) hideSystemUi()\n    }\n\n    private fun hideSystemUi() {\n        WindowCompat.setDecorFitsSystemWindows(window, false)\n        WindowInsetsControllerCompat(window, window.decorView).apply {\n            hide(WindowInsetsCompat.Type.systemBars())\n            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE\n        }\n    }\n}\n''', encoding='utf-8')

s = manifest.read_text(encoding='utf-8')
permission_block = '''    <permission\n        android:name="com.silveton.sampler.permission.DIRECT_ALSA"\n        android:protectionLevel="signature" />\n    <uses-permission android:name="com.silveton.sampler.permission.DIRECT_ALSA" />\n'''
if 'com.silveton.sampler.permission.DIRECT_ALSA' not in s:
    s = s.replace('<manifest xmlns:android="http://schemas.android.com/apk/res/android">\n',
                  '<manifest xmlns:android="http://schemas.android.com/apk/res/android">\n' + permission_block)
manifest.write_text(s, encoding='utf-8')

final_cpp = cpp.read_text(encoding='utf-8')
required = {
    '96k exact': 'constexpr unsigned kRate = 96000U;',
    '2 periods exact': 'constexpr unsigned kPeriods = 2U;',
    '96 frame exact': 'SNDRV_PCM_HW_PARAM_PERIOD_SIZE, kPreferredMinimumPeriodFrames',
    'mixer route': 'Direct ALSA SM-X130 output route configured',
    'control node': '"/dev/snd/controlC0"',
    'route ch1': 'ADDA_DL_CH1 DL1_CH1',
    'route ch2': 'ADDA_DL_CH2 DL1_CH2',
    'dac path': 'DAC In Mux',
    'headphone path': 'HPL Mux',
    'permission recovery': 'recoverRequested_.store(true, std::memory_order_release)',
}
for name, needle in required.items():
    if needle not in final_cpp:
        raise SystemExit(f'missing runtime fix: {name}')
if 'kRates{{96000U, 48000U}}' in final_cpp:
    raise SystemExit('48 kHz probing still present')
if '#include <aaudio/AAudio.h>' in final_cpp or 'AAudioStream_' in final_cpp:
    raise SystemExit('AAudio dependency returned')
if 'SilvetonAudioLifecycle' not in activity.read_text(encoding='utf-8'):
    raise SystemExit('audio lifecycle still on UI thread')
if 'com.silveton.sampler.permission.DIRECT_ALSA' not in manifest.read_text(encoding='utf-8'):
    raise SystemExit('DIRECT_ALSA permission missing')

print('PASS: Direct ALSA runtime repaired (96k/96x2 + mixer route + recovery + nonblocking UI)')
