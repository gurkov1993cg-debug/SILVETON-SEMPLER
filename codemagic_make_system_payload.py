from pathlib import Path
import shutil
import sys
import zipfile

if len(sys.argv) != 3:
    raise SystemExit('usage: codemagic_make_system_payload.py <apk> <out-dir>')

apk = Path(sys.argv[1])
out_dir = Path(sys.argv[2])
if not apk.is_file():
    raise SystemExit(f'APK missing: {apk}')
out_dir.mkdir(parents=True, exist_ok=True)
work = out_dir / '_silveton_system_payload'
if work.exists():
    shutil.rmtree(work)

apk_dst = work / 'system_ext/priv-app/SilvetonSampler/SilvetonSampler.apk'
apk_dst.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(apk, apk_dst)

perm = work / 'system_ext/etc/permissions/silveton-direct-alsa.xml'
perm.parent.mkdir(parents=True, exist_ok=True)
perm.write_text('''<?xml version="1.0" encoding="utf-8"?>\n<permissions>\n    <permission name="com.silveton.sampler.permission.DIRECT_ALSA">\n        <group gid="audio" />\n    </permission>\n</permissions>\n''', encoding='utf-8')

init_rc = work / 'system_ext/etc/init/silveton-performance.rc'
init_rc.parent.mkdir(parents=True, exist_ok=True)
init_rc.write_text('''# SILVETON dedicated live-performance mode.\nservice silveton-performance-on /system/bin/sh /system_ext/bin/silveton-performance-on.sh\n    class late_start\n    user root\n    group root system\n    disabled\n    oneshot\n\nservice silveton-performance-off /system/bin/sh /system_ext/bin/silveton-performance-off.sh\n    class late_start\n    user root\n    group root system\n    disabled\n    oneshot\n\non property:sys.silveton.performance=1\n    start silveton-performance-on\n\non property:sys.silveton.performance=0\n    start silveton-performance-off\n''', encoding='utf-8')

on = work / 'system_ext/bin/silveton-performance-on.sh'
on.parent.mkdir(parents=True, exist_ok=True)
on.write_text(r'''#!/system/bin/sh
set -u
STATE=/data/misc/silveton
mkdir -p "$STATE"
chmod 0700 "$STATE"

save_node() {
    p="$1"; n="$2"
    [ -r "$p" ] && [ ! -f "$STATE/$n" ] && cat "$p" > "$STATE/$n"
}
write_node() {
    p="$1"; v="$2"
    [ -w "$p" ] && echo "$v" > "$p" 2>/dev/null || true
}

if [ ! -f "$STATE/active" ]; then
    save_node /dev/cpuset/background/cpus cpuset_background
    save_node /dev/cpuset/system-background/cpus cpuset_system_background
    save_node /dev/cpuset/restricted/cpus cpuset_restricted
    save_node /sys/devices/system/cpu/cpufreq/policy6/scaling_governor governor_policy6
    save_node /sys/devices/system/cpu/cpufreq/policy7/scaling_governor governor_policy7
    date +%s > "$STATE/active"
fi

write_node /dev/cpuset/background/cpus 0-5
write_node /dev/cpuset/system-background/cpus 0-5
write_node /dev/cpuset/restricted/cpus 0-5
write_node /sys/devices/system/cpu/cpufreq/policy6/scaling_governor performance
write_node /sys/devices/system/cpu/cpufreq/policy7/scaling_governor performance

/system/bin/cmd activity kill-all >/dev/null 2>&1 || true
exit 0
''', encoding='utf-8')
on.chmod(0o755)

off = work / 'system_ext/bin/silveton-performance-off.sh'
off.write_text(r'''#!/system/bin/sh
set -u
STATE=/data/misc/silveton
restore_node() {
    p="$1"; n="$2"
    if [ -w "$p" ] && [ -r "$STATE/$n" ]; then
        cat "$STATE/$n" > "$p" 2>/dev/null || true
    fi
}
restore_node /dev/cpuset/background/cpus cpuset_background
restore_node /dev/cpuset/system-background/cpus cpuset_system_background
restore_node /dev/cpuset/restricted/cpus cpuset_restricted
restore_node /sys/devices/system/cpu/cpufreq/policy6/scaling_governor governor_policy6
restore_node /sys/devices/system/cpu/cpufreq/policy7/scaling_governor governor_policy7
rm -f "$STATE/active" "$STATE/cpuset_background" "$STATE/cpuset_system_background" \
      "$STATE/cpuset_restricted" "$STATE/governor_policy6" "$STATE/governor_policy7"
exit 0
''', encoding='utf-8')
off.chmod(0o755)

policy = work / 'sepolicy/private/silveton_app.te'
policy.parent.mkdir(parents=True, exist_ok=True)
policy.write_text('''type silveton_app, domain;\napp_domain(silveton_app)\n\n# Direct ALSA / mixer access. Linux DAC is still limited by gid=audio.\nallow silveton_app audio_device:dir r_dir_perms;\nallow silveton_app audio_device:chr_file { getattr open read write ioctl lock map };\nallowxperm silveton_app audio_device:chr_file ioctl 0x4100-0x41ff;\n\n# App may only toggle its own dedicated performance property.\nsystem_internal_prop(silveton_perf_prop)\nset_prop(silveton_app, silveton_perf_prop)\n''', encoding='utf-8')

seapp = work / 'sepolicy/private/seapp_contexts'
seapp.write_text('''# Dedicated domain; do not grant this to other applications.\nuser=_app name=com.silveton.sampler domain=silveton_app type=app_data_file levelFrom=all\n''', encoding='utf-8')

prop = work / 'sepolicy/private/property_contexts'
prop.write_text('''sys.silveton.performance    u:object_r:silveton_perf_prop:s0 exact bool\n''', encoding='utf-8')

readme = work / 'INTEGRATION.txt'
readme.write_text('''SILVETON SYSTEM IMAGE PAYLOAD - SM-X130\n\nThis is not a user-installable Magisk module. It is the integration payload for the dedicated Android image.\nThe APK is already placed under system_ext/priv-app.\nIntegrate the SELinux source fragments into the device policy build; they cannot be activated by installing an APK alone.\nLive mode confines background/system-background/restricted work to CPUs 0-5, puts the fast CPU policy in performance mode when available, kills cached app processes once, and restores the previous settings when the sampler suspends/closes.\nCore Android services are deliberately not killed.\n''', encoding='utf-8')

zip_path = out_dir / 'SILVETON-SM-X130-SYSTEM-INTEGRATION.zip'
if zip_path.exists():
    zip_path.unlink()
with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED) as zf:
    for p in sorted(work.rglob('*')):
        if p.is_file():
            zf.write(p, p.relative_to(work).as_posix())

print(zip_path)
