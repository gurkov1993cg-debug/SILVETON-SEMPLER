from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
root_mode = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'
for p in (cpp, root_mode):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

s = cpp.read_text(encoding='utf-8')

# 1) Restore the exact mixer control device used by the physically proven SM-X130 route.
old = '''    static constexpr const char* kPcmDevice = "/dev/snd/pcmC0D0p";\n    static constexpr const char* kAlsaDeviceDirectory = "/dev/snd";\n'''
new = '''    static constexpr const char* kPcmDevice = "/dev/snd/pcmC0D0p";\n    static constexpr const char* kControlDevice = "/dev/snd/controlC0";\n    static constexpr const char* kAlsaDeviceDirectory = "/dev/snd";\n'''
if 'kControlDevice = "/dev/snd/controlC0"' not in s:
    if old not in s:
        raise SystemExit('Direct ALSA constants anchor changed')
    s = s.replace(old, new, 1)

# 2) Restore mixer routing. This was accidentally missing from the rooted APK build.
route_anchor = '''    static int openPcmNode() noexcept {\n        return ::open(kPcmDevice, O_RDWR | O_CLOEXEC);\n    }\n\n'''
route_code = r'''    static std::optional<snd_ctl_elem_id> findControlId(int fd, const char* name) noexcept {
        if (fd < 0 || name == nullptr) return std::nullopt;
        snd_ctl_elem_list list{};
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0 || list.count == 0U) return std::nullopt;
        std::vector<snd_ctl_elem_id> ids(list.count);
        list.offset = 0U;
        list.space = list.count;
        list.pids = ids.data();
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) return std::nullopt;
        const unsigned used = std::min<unsigned>(list.used, static_cast<unsigned>(ids.size()));
        for (unsigned i = 0U; i < used; ++i) {
            if (std::strncmp(reinterpret_cast<const char*>(ids[i].name), name,
                             SNDRV_CTL_ELEM_ID_NAME_MAXLEN) == 0) return ids[i];
        }
        return std::nullopt;
    }

    static bool setIntegerControl(int fd, const char* name, long first, long second, int& errorOut) noexcept {
        const auto id = findControlId(fd, name);
        if (!id.has_value()) { errorOut = -ENOENT; return false; }
        snd_ctl_elem_info info{};
        info.id = *id;
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { errorOut = -errno; return false; }
        if (info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN && info.type != SNDRV_CTL_ELEM_TYPE_INTEGER) {
            errorOut = -EINVAL; return false;
        }
        snd_ctl_elem_value value{};
        value.id = *id;
        const unsigned count = std::min<unsigned>(info.count, 128U);
        for (unsigned i = 0U; i < count; ++i) value.value.integer.value[i] = (i == 0U) ? first : second;
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) < 0) { errorOut = -errno; return false; }
        errorOut = 0;
        return true;
    }

    static bool setEnumControl(int fd, const char* name, const char* itemName, int& errorOut) noexcept {
        const auto id = findControlId(fd, name);
        if (!id.has_value()) { errorOut = -ENOENT; return false; }
        snd_ctl_elem_info info{};
        info.id = *id;
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { errorOut = -errno; return false; }
        if (info.type != SNDRV_CTL_ELEM_TYPE_ENUMERATED || info.value.enumerated.items == 0U) {
            errorOut = -EINVAL; return false;
        }
        unsigned selected = info.value.enumerated.items;
        for (unsigned item = 0U; item < info.value.enumerated.items; ++item) {
            snd_ctl_elem_info itemInfo{};
            itemInfo.id = *id;
            itemInfo.value.enumerated.item = item;
            if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &itemInfo) < 0) { errorOut = -errno; return false; }
            if (std::strncmp(itemInfo.value.enumerated.name, itemName,
                             sizeof(itemInfo.value.enumerated.name)) == 0) {
                selected = item; break;
            }
        }
        if (selected >= info.value.enumerated.items) { errorOut = -ENOENT; return false; }
        snd_ctl_elem_value value{};
        value.id = *id;
        const unsigned count = std::min<unsigned>(info.count, 128U);
        for (unsigned i = 0U; i < count; ++i) value.value.enumerated.item[i] = selected;
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) < 0) { errorOut = -errno; return false; }
        errorOut = 0;
        return true;
    }

    bool configureOutputRouteLocked() noexcept {
        const int fd = ::open(kControlDevice, O_RDWR | O_CLOEXEC);
        if (fd < 0) { lastError_.store(-errno, std::memory_order_relaxed); return false; }

        int error = 0;
        const bool ok =
            setIntegerControl(fd, "ADDA_DL_CH1 DL1_CH1", 1L, 1L, error) &&
            setIntegerControl(fd, "ADDA_DL_CH2 DL1_CH2", 1L, 1L, error) &&
            setEnumControl(fd, "DAC In Mux", "Normal Path", error) &&
            setEnumControl(fd, "HPL Mux", "Audio Playback", error) &&
            setEnumControl(fd, "HPR Mux", "Audio Playback", error);

        if (ok) {
            int volumeError = 0;
            if (!setIntegerControl(fd, "Headset Volume", 12L, 12L, volumeError)) {
                __android_log_print(ANDROID_LOG_WARN, kTag,
                    "SM-X130 Headset Volume route write failed: %d", volumeError);
            }
        }
        const int savedClose = (::close(fd) < 0) ? -errno : 0;
        if (!ok) { lastError_.store(error == 0 ? -EIO : error, std::memory_order_relaxed); return false; }
        if (savedClose != 0) { lastError_.store(savedClose, std::memory_order_relaxed); return false; }
        __android_log_print(ANDROID_LOG_INFO, kTag, "SM-X130 mixer route READY");
        return true;
    }

    static int openPcmNode() noexcept {
        return ::open(kPcmDevice, O_RDWR | O_CLOEXEC);
    }

'''
if 'configureOutputRouteLocked()' not in s:
    if route_anchor not in s:
        raise SystemExit('PCM open anchor changed')
    s = s.replace(route_anchor, route_code, 1)

# 3) Exact 96x2 contract and frame alignment.
s = s.replace(
    '        sw.xfer_align = static_cast<snd_pcm_uframes_t>(std::max(1U, config.periodFrames / 2U));\n',
    '        sw.xfer_align = 1U;\n',
    1)

# 4) Route before PCM open and preserve stage-specific diagnostics.
start = s.find('    bool openLocked() {')
end = s.find('\n    bool startLocked() {', start)
if start < 0 or end < 0:
    raise SystemExit('openLocked block missing')
block = s[start:end]
if 'configureOutputRouteLocked()' not in block:
    marker = '''    bool openLocked() {\n        if (pcmFd_.load(std::memory_order_acquire) >= 0) return true;\n'''
    insert = '''    bool openLocked() {\n        if (pcmFd_.load(std::memory_order_acquire) >= 0) return true;\n        if (!configureOutputRouteLocked()) {\n            const int error = lastError_.load(std::memory_order_relaxed);\n            if (error == -EACCES || error == -EPERM) {\n                recoverRequested_.store(true, std::memory_order_release);\n                setTextError("AUDIO DRIVER: controlC0 permission denied (errno " + std::to_string(-error) + ")");\n            } else {\n                setTextError("AUDIO DRIVER: SM-X130 mixer route failed (errno " + std::to_string(-error) + ")");\n            }\n            return false;\n        }\n'''
    if marker not in s:
        raise SystemExit('openLocked entry anchor changed')
    s = s.replace(marker, insert, 1)

# Replace generic PCM diagnostics with actionable stages.
s = s.replace(
    'setTextError(\n                    std::string("Direct ALSA open/config failed for ") + kPcmDevice\n                    + " (errno " + std::to_string(-error) + ")");',
    'setTextError(\n                    std::string("AUDIO DRIVER: PCM open/config failed ") + kPcmDevice\n                    + " (errno " + std::to_string(-error) + ")"\n                    + (error == -EBUSY ? " [PCM BUSY]" : ""));')

# 5) Preserve explicit XRUN/start diagnostics.
s = s.replace(
    'setTextError("Direct ALSA PREPARE failed (errno " + std::to_string(-error) + ")");',
    'setTextError("AUDIO DRIVER: PREPARE failed (errno " + std::to_string(-error) + ")");')

# 6) Ensure mixer route is part of the source contract.
cpp.write_text(s, encoding='utf-8')

# 7) Fix the hot-plug compile regression introduced by the previous stability patch.
k = root_mode.read_text(encoding='utf-8')
k = k.replace('        return runSu(script)\n', '        return runSu(script, 5)\n')
root_mode.write_text(k, encoding='utf-8')

final = cpp.read_text(encoding='utf-8')
required = [
    'kControlDevice = "/dev/snd/controlC0"',
    'ADDA_DL_CH1 DL1_CH1',
    'ADDA_DL_CH2 DL1_CH2',
    'DAC In Mux',
    'HPL Mux',
    'HPR Mux',
    'Headset Volume',
    'configureOutputRouteLocked()',
    'sw.xfer_align = 1U;',
    'AUDIO DRIVER: PCM open/config failed',
    '[PCM BUSY]',
]
for needle in required:
    if needle not in final:
        raise SystemExit(f'audio-driver contract missing: {needle}')
if 'return runSu(script)\n' in root_mode.read_text(encoding='utf-8'):
    raise SystemExit('one-argument runSu compile regression still present')
print('PASS: SM-X130 Direct ALSA driver hardened (mixer route + exact 96x2 + diagnostics + root hotplug compile fix)')
