from pathlib import Path
import shutil
import sys
import zipfile

out_dir = Path(sys.argv[1])
out_dir.mkdir(parents=True, exist_ok=True)
work = out_dir / '_silveton_audio_boot_layer'
if work.exists():
    shutil.rmtree(work)

(work / 'system/etc/permissions').mkdir(parents=True, exist_ok=True)
(work / 'rootdir/etc/init').mkdir(parents=True, exist_ok=True)
(work / 'sepolicy').mkdir(parents=True, exist_ok=True)

(work / 'system/etc/permissions/silveton-direct-alsa.xml').write_text(
'''<?xml version="1.0" encoding="utf-8"?>
<permissions>
    <permission name="com.silveton.sampler.permission.DIRECT_ALSA">
        <group gid="audio" />
    </permission>
</permissions>
''', encoding='utf-8')

(work / 'rootdir/etc/init/silveton-direct-alsa.rc').write_text(
'''# SILVETON SM-X130 Direct ALSA device-node contract.
# Keep stock ownership/mode; the sampler receives gid=audio through SystemConfig.
on post-fs-data
    chown system audio /dev/snd/controlC0
    chmod 0660 /dev/snd/controlC0
    chown system audio /dev/snd/pcmC0D0p
    chmod 0660 /dev/snd/pcmC0D0p
''', encoding='utf-8')

(work / 'sepolicy/silveton-direct-alsa.te').write_text(
'''# SELinux permits access, while DAC keeps /dev/snd restricted to system:audio 0660.
# Only SILVETON receives gid=audio through its signature permission.
allow untrusted_app_all audio_device:dir { getattr open read search };
allow untrusted_app_all audio_device:chr_file { getattr open read write ioctl lock map };
# ALSA PCM ('A'), control ('U') and raw-MIDI ('W') ioctl families.
allowxperm untrusted_app_all audio_device:chr_file ioctl { 0x4100-0x41ff 0x5500-0x55ff 0x5700-0x57ff };
''', encoding='utf-8')

(work / 'sepolicy/silveton-direct-alsa.rule').write_text(
'''allow untrusted_app_all audio_device dir { getattr open read search }
allow untrusted_app_all audio_device chr_file { getattr open read write ioctl lock map }
allowxperm untrusted_app_all audio_device chr_file ioctl 0x4100-0x41ff
allowxperm untrusted_app_all audio_device chr_file ioctl 0x5500-0x55ff
allowxperm untrusted_app_all audio_device chr_file ioctl 0x5700-0x57ff
''', encoding='utf-8')

(work / 'README.txt').write_text(
'''SILVETON SM-X130 Direct ALSA boot layer

This is NOT a Magisk module.

Target:
  Samsung SM-X130 / X130XXU1AYHB
  /dev/snd/controlC0
  /dev/snd/pcmC0D0p
  Direct ALSA 96 kHz / 96 frames / 2 periods

Integration contract for the dedicated boot image / SILVETON system image:
1. Put system/etc/permissions/silveton-direct-alsa.xml into the system permissions tree before PackageManager starts.
2. Import rootdir/etc/init/silveton-direct-alsa.rc from the boot/init ramdisk.
3. Compile sepolicy/silveton-direct-alsa.te into the device SELinux policy (or apply the equivalent rule file with the boot-image policy builder).
4. Keep SELinux enforcing. Do NOT chmod /dev/snd to 0666 and do NOT use a global permissive policy.

Security model:
- SELinux permits the Android app class to reach audio_device.
- Linux DAC remains system:audio 0660.
- only com.silveton.sampler holding com.silveton.sampler.permission.DIRECT_ALSA receives gid=audio.

The APK still talks directly to ALSA. No AudioFlinger/AAudio fallback is introduced.
''', encoding='utf-8')

zip_path = out_dir / 'SILVETON-SM-X130-DIRECT-ALSA-BOOT-LAYER.zip'
if zip_path.exists():
    zip_path.unlink()
with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED) as zf:
    for p in sorted(work.rglob('*')):
        if p.is_file():
            zf.write(p, p.relative_to(work).as_posix())

print(zip_path)
