#!/usr/bin/env python3
"""Mechanically rewrite 186+/386+ instructions to 8086-safe forms in NASM source.

Handles (in-place, preserving comments/formatting):
  pusha/popa            -> PUSHA86/POPA86           (macros in kernel/cpu8086.inc)
  shl/shr/sar/sal/rol/ror/rcl/rcr reg-or-mem, imm>1 -> SHL_N/SHR_N/... macros
  movzx r16, r/m8       -> mov low,src + xor high,high   (AX/BX/CX/DX dest)
                        -> push ax + mov al,src + xor ah,ah + mov dest,ax + pop ax
                           (SI/DI/BP dest)
Inserts %include "kernel/cpu8086.inc" after the [ORG]/[BITS] header.

Leaves alone and REPORTS for manual fixing:
  imul-imm, push-imm, bt, jcc near, 32-bit registers / dword ops / insw
  movzx whose next instruction is a conditional jump (flag-sensitivity)

Usage: to8086.py FILE [FILE...]
"""
import re, sys

R16 = {'ax': ('al', 'ah'), 'bx': ('bl', 'bh'), 'cx': ('cl', 'ch'), 'dx': ('dl', 'dh')}
IDX16 = ('si', 'di', 'bp')
SHIFTS = {'shl': 'SHL_N', 'sal': 'SHL_N', 'shr': 'SHR_N', 'sar': 'SAR_N',
          'rol': 'ROL_N', 'ror': 'ROR_N', 'rcl': 'RCL_N', 'rcr': 'RCR_N'}
REG32 = re.compile(r'\b(eax|ebx|ecx|edx|esi|edi|ebp|esp)\b')
COND_JMP = re.compile(r'^\s*(j(?!mp)[a-z]+)\b')

def split_comment(line):
    # naive split at first ';' not inside quotes
    q = None
    for i, c in enumerate(line):
        if q:
            if c == q:
                q = None
        elif c in '"\'':
            q = c
        elif c == ';':
            return line[:i], line[i:]
    return line, ''

def parse_imm(tok):
    tok = tok.strip()
    try:
        return int(tok, 0)
    except ValueError:
        return None

def next_code_line(lines, i):
    for j in range(i + 1, min(i + 6, len(lines))):
        code, _ = split_comment(lines[j])
        if code.strip():
            return code
    return ''

def process(path):
    src = open(path, encoding='utf-8', errors='replace').read()
    lines = src.split('\n')
    out, manual, changed = [], [], 0
    inc_done = any('cpu8086.inc' in l for l in lines)
    header_idx = None  # last [BITS]/[ORG]/ORG line index in OUT

    for i, line in enumerate(lines):
        code, comment = split_comment(line)
        stripped = code.strip().lower()
        indent = code[:len(code) - len(code.lstrip())] or '    '

        if re.match(r'^\[?(bits|org)\b', stripped):
            out.append(line)
            header_idx = len(out)
            continue

        # report-only categories
        if REG32.search(code) or re.search(r'\bdword\b|\binsw\b|\boutsw\b', code, re.I):
            manual.append((i + 1, '32-bit operand', code.strip()))
            out.append(line)
            continue
        m = re.match(r'^\s*imul\b(.*)', code, re.I)
        if m and ',' in m.group(1):
            manual.append((i + 1, 'imul-imm', code.strip()))
            out.append(line)
            continue
        m = re.match(r'^\s*push\s+(?:word\s+)?([^\s,;]+)\s*$', code, re.I)
        if m:
            tok = m.group(1).lower()
            regs = ('ax','bx','cx','dx','si','di','bp','sp','cs','ds','es','ss','fs','gs')
            if tok not in regs and not tok.startswith('['):
                manual.append((i + 1, 'push-imm', code.strip()))
            out.append(line)
            continue
        if re.match(r'^\s*bt\b', code, re.I):
            manual.append((i + 1, 'bt', code.strip()))
            out.append(line)
            continue
        if re.match(r'^\s*j[a-z]+\s+near\b', code, re.I) and not stripped.startswith('jmp'):
            manual.append((i + 1, 'jcc-near', code.strip()))
            out.append(line)
            continue

        # pusha/popa (allow leading label)
        m = re.match(r'^(\s*(?:[A-Za-z_.@$][\w.@$]*:\s*)?)(pusha|popa)w?\s*$', code, re.I)
        if m:
            macro = 'PUSHA86' if m.group(2).lower() == 'pusha' else 'POPA86'
            out.append(m.group(1) + macro + comment)
            changed += 1
            continue

        # shift reg/mem, imm>1
        m = re.match(r'^(\s*(?:[A-Za-z_.@$][\w.@$]*:\s*)?)'
                     r'(shl|shr|sar|sal|rol|ror|rcl|rcr)\s+([^,;]+?),\s*([^,;]+?)\s*$',
                     code, re.I)
        if m:
            n = parse_imm(m.group(4))
            if n is not None and n > 1:
                if n > 15:
                    manual.append((i + 1, 'shift>15', code.strip()))
                    out.append(line)
                    continue
                out.append('%s%s %s, %d%s' % (m.group(1), SHIFTS[m.group(2).lower()],
                                              m.group(3).strip(), n, comment))
                changed += 1
                continue

        # movzx r16, r/m8
        m = re.match(r'^(\s*(?:[A-Za-z_.@$][\w.@$]*:\s*)?)movzx\s+(\w+),\s*(.+?)\s*$',
                     code, re.I)
        if m:
            pre, dest, srcop = m.group(1), m.group(2).lower(), m.group(3).strip()
            srcop = re.sub(r'^byte\s+', '', srcop, flags=re.I)
            nxt = next_code_line(lines, i)
            flagrisk = bool(COND_JMP.match(nxt))
            if dest in R16:
                lo, hi = R16[dest]
                if srcop.lower() == lo:
                    repl = ['xor %s, %s' % (hi, hi)]
                else:
                    repl = ['mov %s, %s' % (lo, srcop), 'xor %s, %s' % (hi, hi)]
            elif dest in IDX16:
                if re.search(r'\b(al|ah|ax)\b', srcop):
                    manual.append((i + 1, 'movzx-ax-src', code.strip()))
                    out.append(line)
                    continue
                repl = ['push ax', 'mov al, %s' % srcop, 'xor ah, ah',
                        'mov %s, ax' % dest, 'pop ax']
            else:
                manual.append((i + 1, 'movzx-odd', code.strip()))
                out.append(line)
                continue
            if flagrisk:
                manual.append((i + 1, 'movzx-before-jcc (REVIEW: now clobbers flags)',
                               code.strip()))
            tail = comment if comment else ''
            out.append(pre + repl[0] + (tail and (' ' + tail.strip())))
            for r in repl[1:]:
                out.append(indent + r)
            changed += 1
            continue

        out.append(line)

    if not inc_done and changed and header_idx is not None:
        out.insert(header_idx, '%include "kernel/cpu8086.inc"  ; 8086-safe instruction macros')

    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print('%s: %d sites rewritten, %d manual' % (path, changed, len(manual)))
    for ln, kind, txt in manual:
        print('  MANUAL %s:%d  [%s]  %s' % (path, ln, kind, txt))

if __name__ == '__main__':
    for p in sys.argv[1:]:
        process(p)
