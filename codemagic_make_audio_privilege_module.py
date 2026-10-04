from pathlib import Path
import sys
import zipfile
import shutil

out_dir = Path(sys.argv[1])
out_dir.mkdir(parents=True, exist_ok=True)
work = out_dir / '_silveton_audio_privilege_module'
if work.exists():
    shutil.rmtree(work)
(work / 'system/etc/permissions').mkdir(parents=True, exist_ok=True)

(work / 'module.prop').write_text(
'''id=silveton_direct_alsa\nname=SILVETON SM-X130 Direct ALSA Privilege\nversion=1.0\nversionCode=1\nauthor=SILVETON\ndescription=Grants only the SILVETON sampler signature permission Linux audio GID access and SELinux audio_device access for Direct ALSA on SM-X130.\n''',
encoding='utf-8')

(work / 'system/etc/permissions/silveton-direct-alsa.xml').write_text(
'''<?xml version="1.0" encoding="utf-8"?>\n<permissions>\n    <permission name="com.silveton.sampler.permission.DIRECT_ALSA">\n        <group gid="audio" />\n    </permission>\n</permissions>\n''',
encoding='utf-8')

(work / 'sepolicy.rule').write_text(
'''# App still needs the Linux audio GID above; these rules do not change DAC permissions.\nallow untrusted_app_all audio_device:dir { getattr open read search }\nallow untrusted_app_all audio_device:chr_file { getattr open read write ioctl lock map }\n''',
encoding='utf-8')

(work / 'README.txt').write_text(
'''SILVETON SM-X130 Direct ALSA privilege layer\n\nPurpose:\n- maps com.silveton.sampler.permission.DIRECT_ALSA to Linux gid=audio\n- permits untrusted-app SELinux domains to use audio_device nodes\n- leaves /dev/snd ownership and mode unchanged (system:audio 0660)\n- no AudioFlinger/AAudio fallback\n\nRequirement:\n- Magisk must already be working on the tablet.\n- Install this ZIP from the Magisk app and reboot before testing the SILVETON APK.\n''',
encoding='utf-8')

zip_path = out_dir / 'SILVETON-SM-X130-DIRECT-ALSA-MAGISK.zip'
if zip_path.exists():
    zip_path.unlink()
with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED) as zf:
    for p in sorted(work.rglob('*')):
        if p.is_file():
            zf.write(p, p.relative_to(work).as_posix())

print(zip_path)
