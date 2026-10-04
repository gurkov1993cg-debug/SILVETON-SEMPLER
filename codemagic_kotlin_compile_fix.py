from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
controller = root / 'android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt'
if not controller.is_file():
    raise SystemExit(f'missing {controller}')

s = controller.read_text(encoding='utf-8')
ctor_start = s.find('class SamplerAppController(')
body_anchor = ') : AutoCloseable {'
body_pos = s.find(body_anchor, ctor_start)
if ctor_start < 0 or body_pos < 0:
    raise SystemExit('SamplerAppController class header missing')

pre_body = s[ctor_start:body_pos]
if 'data class MidiStatus(' in pre_body:
    m = re.search(
        r'\n\s*data class MidiStatus\(\n'
        r'\s*val deviceCount: Int,\n'
        r'\s*val lastError: Int,\n'
        r'\s*val activitySerial: Long,\n'
        r'\s*val status: Int,\n'
        r'\s*val data1: Int,\n'
        r'\s*val data2: Int\n'
        r'\s*\)\n',
        pre_body,
    )
    if not m:
        raise SystemExit('malformed MidiStatus block not recognized')

    block = '''\n    data class MidiStatus(\n        val deviceCount: Int,\n        val lastError: Int,\n        val activitySerial: Long,\n        val status: Int,\n        val data1: Int,\n        val data2: Int\n    )\n'''
    pre_body = pre_body[:m.start()] + pre_body[m.end():]
    pre_body = pre_body.replace(
        'private val statusChanged: (String?) -> Unit = {\n},',
        'private val statusChanged: (String?) -> Unit = {},'
    )
    s = s[:ctor_start] + pre_body + s[body_pos:]
    body_pos = s.find(body_anchor, ctor_start)
    insert_at = body_pos + len(body_anchor)
    s = s[:insert_at] + block + s[insert_at:]

body_pos = s.find(body_anchor, ctor_start)
pre_body = s[ctor_start:body_pos]
post_body = s[body_pos:]
if 'data class MidiStatus(' in pre_body:
    raise SystemExit('MidiStatus is still inside constructor parameters')
if 'data class MidiStatus(' not in post_body:
    raise SystemExit('MidiStatus missing from class body')
if 'private val statusChanged: (String?) -> Unit = {},' not in pre_body:
    raise SystemExit('statusChanged default lambda not repaired')

controller.write_text(s, encoding='utf-8')
print('PASS: Kotlin controller structure repaired (MidiStatus moved out of constructor lambda)')
