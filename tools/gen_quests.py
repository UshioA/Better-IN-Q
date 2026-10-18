# -*- coding: utf-8 -*-
# 从 INQ.CT 的三张任务下拉表（中/英/日，CT 条目 30/29/11）生成 src/quests.inc。
# 三张表的行序一致：先是分组标题行（id=0，形如 "0:[任务]"），后面是该组的任务。
import re, sys, xml.etree.ElementTree as ET, os

here = os.path.dirname(os.path.abspath(__file__))
root = ET.parse(os.path.join(here, '..', 'reference_INQ_QCanon.CT')).getroot()

lists = {}
def walk(es):
    for e in es.findall('CheatEntry'):
        if e.findtext('ID') in ('30', '29', '11') and e.findtext('DropDownList'):
            lists[e.findtext('ID')] = [l for l in e.findtext('DropDownList').splitlines() if l.strip()]
        sub = e.find('CheatEntries')
        if sub is not None:
            walk(sub)
walk(root.find('CheatEntries'))
zh, en, ja = lists['30'], lists['29'], lists['11']

def split(line):
    i, name = line.split(':', 1)
    return int(i), name.strip()

def c(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'

# 三张表行序不完全一致（个别任务前后挪过），分组标题的顺序一致：
# 分组按位置对齐，任务按 ID 对齐，显示顺序以中文表为准。
def parse(lines):
    heads, names = [], {}
    for l in lines:
        i, n = split(l)
        if i == 0 and n.startswith('['):
            heads.append(n.strip('[]'))
        else:
            names[i] = n
    return heads, names

hz, nz_ = parse(zh)
he, ne_ = parse(en)
hj, nj_ = parse(ja)
assert len(hz) == len(he) == len(hj)
groups = list(zip(hz, he, hj))
missing = [i for i in nz_ if i not in ne_ or i not in nj_]
if missing:
    print('英/日表里缺的任务（用中文名顶上）:', missing)

quests, g = [], -1
for l in zh:
    i, n = split(l)
    if i == 0 and n.startswith('['):
        g += 1
        continue
    quests.append((g, i, n, ne_.get(i, n), nj_.get(i, n)))

# ---- 拼音（游戏里切不出中文输入法，搜任务靠拼音 / 首字母）----
# 用 Windows 自带的 ICU（icu.dll，Win10 1703 起就有）把每个字转成不带声调的拼音。
# ICU 只给每个字最常见的读音，任务名里出现的多音字另外补上其它读音（多补几个无妨，只是多匹配）。
import ctypes
_icu = ctypes.WinDLL('icu.dll')
_icu.utrans_openU.restype = ctypes.c_void_p
def _u16(s):
    b = s.encode('utf-16-le')
    return (ctypes.c_uint16 * (len(b) // 2 + 1)).from_buffer_copy(b + b'\0\0'), len(b) // 2
_err = ctypes.c_int(0)
_tid, _tn = _u16('Han-Latin; Latin-ASCII')
_trans = _icu.utrans_openU(_tid, _tn, 0, None, 0, None, ctypes.byref(_err))
assert _trans and _err.value <= 0, 'ICU Han-Latin 打不开'
def icu_pinyin(ch):
    buf = (ctypes.c_uint16 * 64)()
    b = ch.encode('utf-16-le')
    ctypes.memmove(buf, b, len(b))
    n = ctypes.c_int32(len(b) // 2); lim = ctypes.c_int32(len(b) // 2); e = ctypes.c_int(0)
    _icu.utrans_transUChars(ctypes.c_void_p(_trans), buf, ctypes.byref(n), 64, 0, ctypes.byref(lim), ctypes.byref(e))
    r = bytes(buf)[:n.value * 2].decode('utf-16-le').strip().lower()
    assert e.value <= 0 and r.isascii() and r.isalpha(), (ch, r)
    return r
POLY = {
    '乐': 'le yue', '了': 'le liao', '会': 'hui kuai', '传': 'chuan zhuan', '便': 'bian pian', '卡': 'ka qia',
    '地': 'di de', '大': 'da dai', '奇': 'qi ji', '属': 'shu zhu', '强': 'qiang jiang', '得': 'de dei',
    '提': 'ti di', '朝': 'chao zhao', '期': 'qi ji', '爪': 'zhua zhao', '的': 'de di', '盛': 'sheng cheng',
    '石': 'shi dan', '给': 'gei ji', '色': 'se shai', '落': 'luo la lao', '薄': 'bao bo', '藏': 'cang zang',
    '行': 'xing hang', '觉': 'jue jiao', '角': 'jiao jue', '调': 'diao tiao', '都': 'dou du', '重': 'zhong chong',
    '长': 'chang zhang', '降': 'jiang xiang', '露': 'lu lou', '只': 'zhi', '干': 'gan', '种': 'zhong',
}
def pinyin(name):
    # 每个字一段，用空格隔开；多音字的几个读音用 / 隔开。字母数字各算一段，标点不要。
    segs = []
    for ch in name:
        if '一' <= ch <= '鿿':
            rs = [icu_pinyin(ch)] + POLY.get(ch, '').split()
            segs.append('/'.join(dict.fromkeys(rs)))
        elif ch.isascii() and ch.isalnum():
            segs.append(ch.lower())
    return ' '.join(segs)

out = os.path.join(here, '..', 'src', 'quests.inc')
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// 由 tools/gen_quests.py 从 INQ.CT 生成，不要手改。\n')
    f.write('// 分组：{中, 英, 日}\n')
    f.write('static const QuestGroup kQuestGroups[] = {\n')
    for g in groups:
        f.write('    {%s, %s, %s},\n' % tuple(c(x) for x in g))
    f.write('};\n\n// 任务：{分组, ID, 中, 英, 日, 中文名的拼音（每字一段，多音字用 / 列出几个读音）}\n')
    f.write('static const QuestInfo kQuests[] = {\n')
    for q in quests:
        f.write('    {%d, %d, %s, %s, %s, %s},\n' % (q[0], q[1], c(q[2]), c(q[3]), c(q[4]), c(pinyin(q[2]))))
    f.write('};\n')
print('groups=%d quests=%d -> %s' % (len(groups), len(quests), out))
