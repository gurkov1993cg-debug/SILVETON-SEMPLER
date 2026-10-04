from pathlib import Path
import sys

root = Path(sys.argv[1])
root_mode = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'
if not root_mode.is_file():
    raise SystemExit(f'missing root runtime: {root_mode}')

k = root_mode.read_text(encoding='utf-8')

# With `set -u`, the generated shell text `idle_$c_...` is parsed as variable
# `c_`, not variable `c` followed by an underscore. On Android /system/bin/sh
# that becomes `c_: parameter not set`. Force a braced shell variable instead.
bad = "idle_${'$'}c_${'$'}(basename"
good = "idle_${'$'}{c}_${'$'}(basename"

count = k.count(bad)
if count == 0 and good not in k:
    raise SystemExit('CPU idle key shell-variable anchor missing')

k = k.replace(bad, good)
root_mode.write_text(k, encoding='utf-8')

final = root_mode.read_text(encoding='utf-8')
if bad in final:
    raise SystemExit('unsafe $c_ shell expansion still present')
if good not in final:
    raise SystemExit('braced ${c} shell expansion missing')

print(f'PASS: Android shell nounset fix applied ({count} unsafe $c_ expansion(s) corrected)')
