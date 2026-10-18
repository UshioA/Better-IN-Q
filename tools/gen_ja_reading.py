# -*- coding: utf-8 -*-
# 给 src/quests.inc 的每一行任务补两个字段：日文名的假名读音、罗马字读音（pykakasi）。
# 这样搜任务时，玩家打假名（でんせつ）或罗马音（densetsu / kokuryuu）也能搜到日文名带汉字的任务。
#
# 用法：py -3 tools/gen_ja_reading.py
# 依赖：pip install pykakasi
# 说明：幂等。已经补过读音的行会重算覆盖。gen_quests.py 重跑之后要再跑一次这个。
import io, os, re, sys

try:
    from pykakasi import Converter as _Converter
    conv = _Converter()
except ImportError:                     # pykakasi 2.x
    from pykakasi import kakasi
    conv = kakasi()

here = os.path.dirname(os.path.abspath(__file__))
inc = os.path.join(here, '..', 'src', 'quests.inc')

ESC = {'n': '\n', 't': '\t', '"': '"', '\\': '\\'}


def parse_line(line):
    """把 '{1, 552, "中", "英", "日", "拼音"},' 拆成 [(kind, value), ...]。不是任务行返回 None。"""
    s = line.strip()
    if not (s.startswith('{') and s.endswith('},')):
        return None
    s = s[1:-2]
    i, n, items = 0, len(s), []
    while i < n:
        ch = s[i]
        if ch in ' ,':
            i += 1
        elif ch == '"':
            i += 1
            buf = []
            while True:
                c = s[i]
                if c == '\\':
                    buf.append(ESC.get(s[i + 1], s[i + 1]))
                    i += 2
                elif c == '"':
                    i += 1
                    break
                else:
                    buf.append(c)
                    i += 1
            items.append(('s', ''.join(buf)))
        else:
            j = i
            while j < n and s[j] not in ' ,':
                j += 1
            items.append(('n', s[i:j]))
            i = j
    if len(items) < 3 or items[0][0] != 'n' or items[1][0] != 'n':
        return None
    return items


def c(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def kana_of(name):
    return ''.join(x.get('hira', '') for x in conv.convert(name))


def romaji_of(name):
    r = ''.join(x.get('hepburn', '') for x in conv.convert(name)).lower()
    # 只留 a-z0-9：' ・ ~ 这些在搜索时没意义
    return re.sub(r'[^a-z0-9]', '', r)


def main():
    with io.open(inc, encoding='utf-8', newline='') as f:
        lines = f.read().split('\n')
    out, done = [], 0
    for line in lines:
        items = parse_line(line)
        if items is None:
            if line.startswith('// 由 tools/gen_quests.py'):
                line = '// 由 tools/gen_quests.py 从 INQ.CT 生成，再由 tools/gen_ja_reading.py 补日文读音，不要手改。'
            elif line.startswith('// 任务：{分组, ID, 中, 英, 日, 中文名的拼音'):
                line = ('// 任务：{分组, ID, 中, 英, 日, 中文名的拼音（每字一段，多音字用 / 列出几个读音）,'
                        ' 日文名的假名读音, 日文名的罗马字读音（pykakasi）}')
            out.append(line)
            continue
        group, qid = items[0][1], items[1][1]
        strs = [v for _, v in items[2:]]
        zh, en, ja, py = strs[0], strs[1], strs[2], strs[3]
        out.append('    {%s, %s, %s, %s, %s, %s, %s, %s},'
                   % (group, qid, c(zh), c(en), c(ja), c(py), c(kana_of(ja)), c(romaji_of(ja))))
        done += 1
    with io.open(inc, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))
    print('补了 %d 个任务的读音 -> %s' % (done, os.path.normpath(inc)))
    if done < 500:
        sys.exit(1)


if __name__ == '__main__':
    main()
