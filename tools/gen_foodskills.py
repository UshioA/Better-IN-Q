# -*- coding: utf-8 -*-
# 从 IN-Q 的 CT（「Various editing script」里 --[[ ]] 注释块）生成 src/foodskills.inc。
# 注释块里是三段同构列表，每段 57 行「名字 编号」：日文、英文、中文，编号 -1~56（37 缺号）。
#
#   python tools/gen_foodskills.py [INQ.CT 路径]
#
# 默认读仓库根目录的 reference_INQ_QCanon.CT（和 gen_quests.py 同一份，不随仓库分发）。
import os
import re
import sys
import xml.etree.ElementTree as ET

here = os.path.dirname(os.path.abspath(__file__))
ct = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'reference_INQ_QCanon.CT')

root = ET.parse(ct).getroot()

script = None
for ce in root.iter('CheatEntry'):
    if (ce.findtext('Description') or '').strip('"') == 'Various editing script':
        script = ce.findtext('AssemblerScript')
if script is None:
    sys.exit('.CT 里找不到 Various editing script')
block = script.split('--[[', 1)[1].rsplit(']]', 1)[0].strip()

# 三段之间用空行隔开
blocks = [b.strip().splitlines() for b in re.split(r'\n\s*\n', block) if b.strip()]
if len(blocks) != 3:
    sys.exit('注释块应该是三段（日/英/中），实际 %d 段' % len(blocks))


def parse(lines):
    out = {}
    for l in lines:
        m = re.match(r'^(.*)\s+(-?\d+)$', l.strip())
        if not m:
            sys.exit('看不懂的行: %r' % l)
        out[int(m.group(2))] = m.group(1).strip()
    return out


ja, en, zh = parse(blocks[0]), parse(blocks[1]), parse(blocks[2])
assert ja.keys() == en.keys() == zh.keys(), '三段编号对不上'
ids = sorted(zh)


def c(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


out = os.path.join(here, '..', 'src', 'foodskills.inc')
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// 由 tools/gen_foodskills.py 从 IN-Q 的 CT 生成，不要手改。\n')
    f.write('// {编号, 中, 英, 日}；编号 37 原表就没有\n')
    f.write('static const FoodSkillName kFoodSkills[] = {\n')
    for i in ids:
        f.write('    {%d, %s, %s, %s},\n' % (i, c(zh[i]), c(en[i]), c(ja[i])))
    f.write('};\n')
print('food skills=%d -> %s' % (len(ids), out))
