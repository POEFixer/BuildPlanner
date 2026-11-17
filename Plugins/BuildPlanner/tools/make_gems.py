#!/usr/bin/env python3
# BuildPlanner - the gem table generator
# make_gems.py - writes data/gems.json (the format core/GemData.h reads) from Path of Building
# Community (PoE2) data: src/Data/Gems.lua for the gems, and src/Data/Skills/*.lua (the same commit)
# for the colour of each gem's granted effect (Path of Building colours a gem by grantedEffect.color:
# 1 str, 2 dex, 3 int) and for the number of active skills a gem adds to a group's list ("actives",
# GemInfo::activeSkills: what a <Skill mainActiveSkill> counts). Standard library only.
#
#   python -I tools/make_gems.py --pob <PathOfBuilding-PoE2 checkout or the src folder> \
#       [--skills <folder with the Skills/*.lua files>] --commit <hash> [--commit-date <iso>] \
#       [--out data/gems.json] [--check]
#
# --skills defaults to <src>/Data/Skills; fewer than 100 skills read is an error. A gem whose granted
# effect has no colour in the Skills files gets the colour of its single highest attribute requirement
# (reqStr / reqDex / reqInt), "none" when two tie or all are 0. --check compares with the existing file
# and exits 1 when it differs.
#
# Rows: one per gem id as Lua resolves Gems.lua (a key written twice keeps its LAST value), keyed by the
# gem's `gameId` (the BaseItemTypes id the build XML and the game use; Path of Building writes both the
# "Metadata/Items/Gems/" and the "Metadata/Items/Gem/" spelling and so does this file). Sorted by id.
import argparse
import json
import os
import re
import sys


class LuaError(Exception):
    pass


_TOKEN = re.compile(
    r'\s+|--\[\[.*?\]\]|--[^\n]*'                       # whitespace, comments
    r'|(?P<str>"(?:[^"\\\n]|\\.)*")'                   # "string"
    r'|(?P<num>-?(?:0[xX][0-9a-fA-F]+|\d+(?:\.\d*)?(?:[eE][-+]?\d+)?))'
    r'|(?P<name>[A-Za-z_][A-Za-z0-9_]*)'
    r'|(?P<op>[{}\[\]=,;])',
    re.S)

_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'", '\n': '\n', 'a': '\a', 'b': '\b',
            'f': '\f', 'v': '\v'}


def _unescape(body):
    out = []
    i = 0
    while i < len(body):
        c = body[i]
        if c != '\\':
            out.append(c)
            i += 1
            continue
        i += 1
        if i >= len(body):
            raise LuaError('a string ends in a backslash')
        e = body[i]
        if e.isdigit():
            j = i
            while j < len(body) and j < i + 3 and body[j].isdigit():
                j += 1
            out.append(chr(int(body[i:j])))
            i = j
            continue
        if e not in _ESCAPES:
            raise LuaError('unknown escape \\' + e)
        out.append(_ESCAPES[e])
        i += 1
    return ''.join(out)


def _tokens(text):
    pos = 0
    while pos < len(text):
        m = _TOKEN.match(text, pos)
        if not m:
            raise LuaError('unexpected text at offset %d: %r' % (pos, text[pos:pos + 30]))
        pos = m.end()
        if m.group('str') is not None:
            yield ('str', _unescape(m.group('str')[1:-1]))
        elif m.group('num') is not None:
            t = m.group('num')
            yield ('num', int(t, 16) if t.lower().startswith(('0x', '-0x')) else (float(t) if any(c in t for c in '.eE') else int(t)))
        elif m.group('name') is not None:
            yield ('name', m.group('name'))
        elif m.group('op') is not None:
            yield ('op', m.group('op'))


class _Parser:
    """The data subset of Lua that Gems.lua uses: `return { ... }` with nested tables, strings, numbers,
    true / false / nil. Tables become dicts (a key written twice keeps its last value, as in Lua)."""

    def __init__(self, text):
        self.toks = list(_tokens(text))
        self.i = 0

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else ('eof', None)

    def take(self, kind=None, value=None):
        t = self.peek()
        if kind and t[0] != kind or value is not None and t[1] != value:
            raise LuaError('expected %s %r, found %r' % (kind, value, t))
        self.i += 1
        return t

    def value(self):
        t = self.peek()
        if t == ('op', '{'):
            return self.table()
        if t[0] in ('str', 'num'):
            self.i += 1
            return t[1]
        if t[0] == 'name' and t[1] in ('true', 'false', 'nil'):
            self.i += 1
            return {'true': True, 'false': False, 'nil': None}[t[1]]
        raise LuaError('unexpected %r' % (t,))

    def table(self):
        self.take('op', '{')
        out = {}
        n = 0
        while self.peek() != ('op', '}'):
            t = self.peek()
            if t == ('op', '['):
                self.take()
                key = self.value()
                self.take('op', ']')
                self.take('op', '=')
                out[key] = self.value()
            elif t[0] == 'name' and self.i + 1 < len(self.toks) and self.toks[self.i + 1] == ('op', '='):
                self.i += 2
                out[t[1]] = self.value()
            else:
                n += 1
                out[n] = self.value()
            if self.peek() in (('op', ','), ('op', ';')):
                self.take()
            elif self.peek() != ('op', '}'):
                raise LuaError('expected , or } but found %r' % (self.peek(),))
        self.take('op', '}')
        return out

    def chunk(self):
        self.take('name', 'return')
        v = self.value()
        if self.peek()[0] != 'eof':
            raise LuaError('text after the returned table')
        return v


def parse_gems_lua(text):
    data = _Parser(text).chunk()
    if not isinstance(data, dict):
        raise LuaError('Gems.lua does not return a table')
    return data


_SKILL_HEAD = re.compile(r'^skills\["([^"\\]+)"\]\s*=\s*\{', re.M)
_SKILL_COLOR = re.compile(r'^\tcolor\s*=\s*(\d+)\s*,', re.M)
_SKILL_FLAGS = {key: re.compile(r'^\t' + key + r'\s*=\s*true\s*,', re.M)
                for key in ('support', 'hideFromSideBar', 'hasGlobalEffect')}


def skill_info(folder):
    """grantedEffect id -> {'color': n or None, 'support', 'hideFromSideBar', 'hasGlobalEffect'}, from every
    *.lua in `folder` (the top-level fields of each `skills["Id"] = {` block: one tab deep)."""
    skills = {}
    if not folder or not os.path.isdir(folder):
        return skills
    for name in sorted(os.listdir(folder)):
        if not name.endswith('.lua'):
            continue
        with open(os.path.join(folder, name), encoding='utf-8') as f:
            text = f.read()
        heads = list(_SKILL_HEAD.finditer(text))
        for k, h in enumerate(heads):
            end = heads[k + 1].start() if k + 1 < len(heads) else len(text)
            m = _SKILL_COLOR.search(text, h.end(), end)
            entry = {'color': int(m.group(1)) if m else None}
            for key, rx in _SKILL_FLAGS.items():
                entry[key] = rx.search(text, h.end(), end) is not None
            skills[h.group(1)] = entry
    return skills


def granted_effects(gem):
    """Path of Building's gem.grantedEffectList as ids (Modules/Data.lua setupGem): the granted effect,
    additionalGrantedEffectId1.., the Barbs triggered skill, then grantedEffectDisplayOrder applied."""
    effects = [gem.get('grantedEffectId')]
    i = 1
    while gem.get('additionalGrantedEffectId%d' % i):
        effects.append(gem['additionalGrantedEffectId%d' % i])
        i += 1
    name = gem.get('name') or ''
    if name[:7] == 'Barbs I':
        effects.append('TriggeredBarbsPlayer' + ('Two' if name == 'Barbs II' else 'Three' if name == 'Barbs III' else ''))
    order = gem.get('grantedEffectDisplayOrder')
    if isinstance(order, dict) and order:
        order = [order[k] for k in sorted(order)]
        reordered, used = [], set()
        for i, o in enumerate(order):
            if isinstance(o, int) and 0 <= o < len(effects) and o not in used:
                reordered.append(effects[o])
                used.add(o)
            else:
                reordered.append(effects[i] if i < len(effects) else None)
                used.add(i)
        reordered += [e for i, e in enumerate(effects) if i not in used]
        effects = reordered
    return effects


def gem_actives(gem, skills, support):
    """How many active skills one enabled copy of the gem adds to a group's list in Path of Building
    (Modules/CalcSetup.lua: each entry of grantedEffectList, up to the first hole as ipairs walks it,
    that is no support, not hideFromSideBar, and not a global effect left off: enableGlobal1 defaults
    to on, enableGlobal2 to off). An effect the Skills files do not define is no entry (Lua drops the
    nil). A gem whose own granted effect is unknown keeps the default (support ? 0 : 1)."""
    effects = granted_effects(gem)
    if effects[0] not in skills:
        return 0 if support else 1
    count = 0
    index = 0
    for effect in effects:
        if effect is None:
            break
        if effect not in skills:
            continue   # table.insert(list, nil) appends nothing
        index += 1
        s = skills[effect]
        if s['support'] or s['hideFromSideBar']:
            continue
        if s['hasGlobalEffect'] and index != 1:
            continue
        count += 1
    return count


_COLOR_NAMES = {1: 'str', 2: 'dex', 3: 'int'}


def gem_color(gem, skills):
    effect = gem.get('grantedEffectId')
    if effect in skills and skills[effect]['color'] is not None:
        return _COLOR_NAMES.get(skills[effect]['color'], 'none'), True
    reqs = [(int(gem.get('reqStr') or 0), 'str'), (int(gem.get('reqDex') or 0), 'dex'), (int(gem.get('reqInt') or 0), 'int')]
    best = max(r[0] for r in reqs)
    if best <= 0 or sum(1 for r in reqs if r[0] == best) != 1:
        return 'none', False
    return next(r[1] for r in reqs if r[0] == best), False


def build_rows(gems, skills):
    rows = {}
    by_reqs = 0
    for key, gem in gems.items():
        if not isinstance(gem, dict):
            raise LuaError('the entry %r is not a table' % (key,))
        game_id = gem.get('gameId') or key
        name = gem.get('name')
        if not isinstance(game_id, str) or not game_id.startswith('Metadata/') or not isinstance(name, str) or not name.strip():
            raise LuaError('the entry %r has no gameId or name' % (key,))
        tags = gem.get('tags') if isinstance(gem.get('tags'), dict) else {}
        support = tags.get('support') is True or gem.get('gemType') == 'Support'
        color, from_skill = gem_color(gem, skills)
        if not from_skill:
            by_reqs += 1
        row = {'id': game_id}
        if isinstance(gem.get('variantId'), str) and gem['variantId']:
            row['variant'] = gem['variantId']
        row['name'] = name.strip()
        row['support'] = bool(support)
        row['color'] = color
        if isinstance(gem.get('grantedEffectId'), str) and gem['grantedEffectId']:
            row['skill'] = gem['grantedEffectId']
        if isinstance(gem.get('gemFamily'), str) and gem['gemFamily']:
            row['family'] = gem['gemFamily']
        actives = gem_actives(gem, skills, support)
        if actives != (0 if support else 1):
            row['actives'] = actives
        if game_id in rows:
            raise LuaError('two entries share the gameId %s' % game_id)
        rows[game_id] = row
    return [rows[k] for k in sorted(rows)], by_reqs


def render(rows, meta):
    lines = ['{', '  "bp_meta": ' + json.dumps(meta, ensure_ascii=False, sort_keys=False) + ',', '  "gems": [']
    for k, row in enumerate(rows):
        lines.append('    ' + json.dumps(row, ensure_ascii=False) + (',' if k + 1 < len(rows) else ''))
    lines += ['  ]', '}', '']
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pob', required=True, help='Path of Building (PoE2) checkout or its src folder')
    ap.add_argument('--skills', help='folder with Skills/*.lua (default: <src>/Data/Skills)')
    ap.add_argument('--commit', required=True)
    ap.add_argument('--commit-date', default='')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'gems.json'))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()

    src = a.pob if os.path.isfile(os.path.join(a.pob, 'Data', 'Gems.lua')) else os.path.join(a.pob, 'src')
    gems_lua = os.path.join(src, 'Data', 'Gems.lua')
    with open(gems_lua, encoding='utf-8') as f:
        gems = parse_gems_lua(f.read())
    skills_dir = a.skills or os.path.join(src, 'Data', 'Skills')
    skills = skill_info(skills_dir)
    if len(skills) < 100:
        sys.exit('%s: only %d skills read; the Skills/*.lua files of the same commit are needed (--skills)'
                 % (skills_dir, len(skills)))
    rows, by_reqs = build_rows(gems, skills)
    if len(rows) < 100:
        sys.exit('only %d gems: refusing to write' % len(rows))
    meta = {
        'source': 'Path of Building Community (PoE2) src/Data/Gems.lua; colours and active skill counts '
                  'from src/Data/Skills/*.lua',
        'commit': a.commit,
        'commitDate': a.commit_date,
        'count': len(rows),
        'colorsFromRequirements': by_reqs,
        'generator': 'tools/make_gems.py',
    }
    text = render(rows, meta)
    out = os.path.abspath(a.out)
    if a.check:
        try:
            with open(out, encoding='utf-8', newline='') as f:
                same = f.read().replace('\r\n', '\n') == text   # a checkout may carry CRLF
        except OSError:
            same = False
        print('%s: %s' % (out, 'up to date' if same else 'DIFFERS'))
        sys.exit(0 if same else 1)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    supports = sum(1 for r in rows if r['support'])
    print('%s: %d gems (%d supports), %d coloured by requirements, %d with another active skill count, %d skills read'
          % (out, len(rows), supports, by_reqs, sum(1 for r in rows if 'actives' in r), len(skills)))


if __name__ == '__main__':
    main()
