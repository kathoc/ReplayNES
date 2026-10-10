#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""A small two-pass 6502 assembler for the ReplayNES Test Cartridge (no dependencies).

Syntax (a ca65-like subset):
  ; comment                      label:   @local:   (local labels are scoped to the last global)
  NAME = expr                    constant (may be redefined only with the same value)
  .org expr                      set the program counter (the image grows / pads with .fill byte)
  .byte expr|"text", ...         .word expr, ...      .res count[, value]     .align n[, value]
  .assert expr, "message"        fails assembly when expr is 0 (checked in the final pass)
  .export name, ...              symbols written to the symbol table returned by assemble()
  .delay n                       busy-wait exactly n CPU cycles (clobbers X and flags)
  .include "file"                textual include (path relative to the including file)
  .macro name arg, ...  / .endmacro   textual macro; inside, @@x labels are unique per expansion

Operands: #imm  zp/abs  zp,x  abs,y  (zp,x)  (zp),y  (abs)  a  — `a:expr` forces absolute.
Expressions: $hex %bin dec 'c' symbols, * (current pc), unary - ~ < (low byte) > (high byte) !,
binary * / % + - << >> & ^ | == != < > <= >= && ||, parentheses. Forward references are
assumed absolute (2-byte) so instruction sizes never change between passes.
"""
import os
import re
import sys

OPCODES = {
    # mnemonic: {mode: opcode}
    'ADC': {'imm': 0x69, 'zp': 0x65, 'zpx': 0x75, 'abs': 0x6D, 'absx': 0x7D, 'absy': 0x79, 'indx': 0x61, 'indy': 0x71},
    'AND': {'imm': 0x29, 'zp': 0x25, 'zpx': 0x35, 'abs': 0x2D, 'absx': 0x3D, 'absy': 0x39, 'indx': 0x21, 'indy': 0x31},
    'ASL': {'acc': 0x0A, 'zp': 0x06, 'zpx': 0x16, 'abs': 0x0E, 'absx': 0x1E},
    'BCC': {'rel': 0x90}, 'BCS': {'rel': 0xB0}, 'BEQ': {'rel': 0xF0}, 'BMI': {'rel': 0x30},
    'BNE': {'rel': 0xD0}, 'BPL': {'rel': 0x10}, 'BVC': {'rel': 0x50}, 'BVS': {'rel': 0x70},
    'BIT': {'zp': 0x24, 'abs': 0x2C},
    'BRK': {'imp': 0x00},
    'CLC': {'imp': 0x18}, 'CLD': {'imp': 0xD8}, 'CLI': {'imp': 0x58}, 'CLV': {'imp': 0xB8},
    'CMP': {'imm': 0xC9, 'zp': 0xC5, 'zpx': 0xD5, 'abs': 0xCD, 'absx': 0xDD, 'absy': 0xD9, 'indx': 0xC1, 'indy': 0xD1},
    'CPX': {'imm': 0xE0, 'zp': 0xE4, 'abs': 0xEC},
    'CPY': {'imm': 0xC0, 'zp': 0xC4, 'abs': 0xCC},
    'DEC': {'zp': 0xC6, 'zpx': 0xD6, 'abs': 0xCE, 'absx': 0xDE},
    'DEX': {'imp': 0xCA}, 'DEY': {'imp': 0x88},
    'EOR': {'imm': 0x49, 'zp': 0x45, 'zpx': 0x55, 'abs': 0x4D, 'absx': 0x5D, 'absy': 0x59, 'indx': 0x41, 'indy': 0x51},
    'INC': {'zp': 0xE6, 'zpx': 0xF6, 'abs': 0xEE, 'absx': 0xFE},
    'INX': {'imp': 0xE8}, 'INY': {'imp': 0xC8},
    'JMP': {'abs': 0x4C, 'ind': 0x6C},
    'JSR': {'abs': 0x20},
    'LDA': {'imm': 0xA9, 'zp': 0xA5, 'zpx': 0xB5, 'abs': 0xAD, 'absx': 0xBD, 'absy': 0xB9, 'indx': 0xA1, 'indy': 0xB1},
    'LDX': {'imm': 0xA2, 'zp': 0xA6, 'zpy': 0xB6, 'abs': 0xAE, 'absy': 0xBE},
    'LDY': {'imm': 0xA0, 'zp': 0xA4, 'zpx': 0xB4, 'abs': 0xAC, 'absx': 0xBC},
    'LSR': {'acc': 0x4A, 'zp': 0x46, 'zpx': 0x56, 'abs': 0x4E, 'absx': 0x5E},
    'NOP': {'imp': 0xEA},
    'ORA': {'imm': 0x09, 'zp': 0x05, 'zpx': 0x15, 'abs': 0x0D, 'absx': 0x1D, 'absy': 0x19, 'indx': 0x01, 'indy': 0x11},
    'PHA': {'imp': 0x48}, 'PHP': {'imp': 0x08}, 'PLA': {'imp': 0x68}, 'PLP': {'imp': 0x28},
    'ROL': {'acc': 0x2A, 'zp': 0x26, 'zpx': 0x36, 'abs': 0x2E, 'absx': 0x3E},
    'ROR': {'acc': 0x6A, 'zp': 0x66, 'zpx': 0x76, 'abs': 0x6E, 'absx': 0x7E},
    'RTI': {'imp': 0x40}, 'RTS': {'imp': 0x60},
    'SBC': {'imm': 0xE9, 'zp': 0xE5, 'zpx': 0xF5, 'abs': 0xED, 'absx': 0xFD, 'absy': 0xF9, 'indx': 0xE1, 'indy': 0xF1},
    'SEC': {'imp': 0x38}, 'SED': {'imp': 0xF8}, 'SEI': {'imp': 0x78},
    'STA': {'zp': 0x85, 'zpx': 0x95, 'abs': 0x8D, 'absx': 0x9D, 'absy': 0x99, 'indx': 0x81, 'indy': 0x91},
    'STX': {'zp': 0x86, 'zpy': 0x96, 'abs': 0x8E},
    'STY': {'zp': 0x84, 'zpx': 0x94, 'abs': 0x8C},
    'TAX': {'imp': 0xAA}, 'TAY': {'imp': 0xA8}, 'TSX': {'imp': 0xBA}, 'TXA': {'imp': 0x8A},
    'TXS': {'imp': 0x9A}, 'TYA': {'imp': 0x98},
}
SIZE = {'imp': 1, 'acc': 1, 'imm': 2, 'zp': 2, 'zpx': 2, 'zpy': 2, 'abs': 3, 'absx': 3, 'absy': 3,
        'ind': 3, 'indx': 2, 'indy': 2, 'rel': 2}


class AsmError(Exception):
    pass


class Undefined(Exception):
    pass


# ----------------------------------------------------------------------------- expressions
TOKEN_RE = re.compile(r"""\s*(?:
    (?P<hex>\$[0-9A-Fa-f]+) | (?P<bin>%[01]+) | (?P<dec>\d+) | (?P<chr>'(?:\\.|[^'])') |
    (?P<sym>[@A-Za-z_][A-Za-z0-9_@.]*) |
    (?P<op><<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^~<>()!])
)""", re.X)


def tokenize(s):
    toks, pos = [], 0
    s = s.rstrip()
    while pos < len(s):
        m = TOKEN_RE.match(s, pos)
        if not m or m.end() == pos:
            raise AsmError("bad expression near '%s'" % s[pos:])
        pos = m.end()
        kind = m.lastgroup
        v = m.group(kind)
        if kind == 'hex':
            toks.append(('num', int(v[1:], 16)))
        elif kind == 'bin':
            toks.append(('num', int(v[1:], 2)))
        elif kind == 'dec':
            toks.append(('num', int(v)))
        elif kind == 'chr':
            body = v[1:-1]
            toks.append(('num', ord(body[1] if body.startswith('\\') else body)))
        elif kind == 'sym':
            toks.append(('sym', v))
        else:
            toks.append(('op', v))
    return toks


BINARY = {'||': 1, '&&': 2, '|': 3, '^': 4, '&': 5, '==': 6, '!=': 6, '<': 7, '>': 7, '<=': 7, '>=': 7,
          '<<': 8, '>>': 8, '+': 9, '-': 9, '*': 10, '/': 10, '%': 10}


class Expr:
    def __init__(self, toks, lookup):
        self.t, self.i, self.lookup = toks, 0, lookup

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else (None, None)

    def take(self):
        tok = self.peek()
        self.i += 1
        return tok

    def parse(self, minprec=0):
        lhs = self.unary()
        while True:
            kind, v = self.peek()
            if kind != 'op' or v not in BINARY or BINARY[v] < minprec:
                return lhs
            self.take()
            rhs = self.parse(BINARY[v] + 1)
            lhs = {'||': lambda a, b: int(bool(a) or bool(b)), '&&': lambda a, b: int(bool(a) and bool(b)),
                   '|': lambda a, b: a | b, '^': lambda a, b: a ^ b, '&': lambda a, b: a & b,
                   '==': lambda a, b: int(a == b), '!=': lambda a, b: int(a != b), '<': lambda a, b: int(a < b),
                   '>': lambda a, b: int(a > b), '<=': lambda a, b: int(a <= b), '>=': lambda a, b: int(a >= b),
                   '<<': lambda a, b: a << b, '>>': lambda a, b: a >> b, '+': lambda a, b: a + b,
                   '-': lambda a, b: a - b, '*': lambda a, b: a * b,
                   '/': lambda a, b: a // b, '%': lambda a, b: a % b}[v](lhs, rhs)

    def unary(self):
        kind, v = self.take()
        if kind == 'num':
            return v
        if kind == 'sym':
            return self.lookup(v)
        if kind == 'op':
            if v == '(':
                r = self.parse()
                if self.take() != ('op', ')'):
                    raise AsmError("missing ')'")
                return r
            if v == '-':
                return -self.unary()
            if v == '~':
                return ~self.unary() & 0xFFFF
            if v == '!':
                return int(not self.unary())
            if v == '<':                # ca65 style: applies to the whole rest of the expression
                return self.parse() & 0xFF
            if v == '>':
                return (self.parse() >> 8) & 0xFF
            if v == '*':
                return self.lookup('*')
        raise AsmError("unexpected token %r" % (v,))


# ----------------------------------------------------------------------------- assembler
def split_args(s):
    """Split on commas outside quotes and parentheses."""
    out, cur, depth, q = [], '', 0, None
    for ch in s:
        if q:
            cur += ch
            if ch == q:
                q = None
        elif ch in '"\'':
            q = ch
            cur += ch
        elif ch == '(':
            depth += 1
            cur += ch
        elif ch == ')':
            depth -= 1
            cur += ch
        elif ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def strip_comment(line):
    q = None
    for i, ch in enumerate(line):
        if q:
            if ch == q:
                q = None
        elif ch in '"\'':
            # a lone ' inside 'x' char literal
            q = ch
        elif ch == ';':
            return line[:i]
    return line


class Assembler:
    def __init__(self, fill=0xFF):
        self.fill = fill

    def load(self, path):
        lines = []
        self._load(path, lines)
        # Expand macros once (textual).
        self.lines = self._expand_macros(lines)

    def _load(self, path, out):
        with open(path, encoding='utf-8') as f:
            for n, raw in enumerate(f, 1):
                text = strip_comment(raw.rstrip('\n'))
                m = re.match(r'\s*\.include\s+"([^"]+)"\s*$', text, re.I)
                if m:
                    self._load(os.path.join(os.path.dirname(path), m.group(1)), out)
                    continue
                out.append((path, n, text))

    def _expand_macros(self, lines):
        macros, out, i = {}, [], 0
        counter = [0]
        while i < len(lines):
            path, n, text = lines[i]
            m = re.match(r'\s*\.macro\s+(\w+)\s*(.*)$', text, re.I)
            if m:
                name, params = m.group(1).lower(), [p.strip() for p in m.group(2).split(',') if p.strip()]
                body, i = [], i + 1
                while i < len(lines) and not re.match(r'\s*\.endmacro\b', lines[i][2], re.I):
                    body.append(lines[i])
                    i += 1
                macros[name] = (params, body)
                i += 1
                continue
            out.append(lines[i])
            i += 1
        # expansion pass (nested macros expanded repeatedly)
        for _ in range(8):
            changed, res = False, []
            for path, n, text in out:
                m = re.match(r'\s*((?:[@A-Za-z_]\w*:)?)\s*(\w+)\s*(.*)$', text)
                if m and m.group(2).lower() in macros:
                    params, body = macros[m.group(2).lower()]
                    args = split_args(m.group(3))
                    if len(args) != len(params):
                        raise AsmError('%s:%d: macro %s expects %d args' % (path, n, m.group(2), len(params)))
                    counter[0] += 1
                    if m.group(1):
                        res.append((path, n, m.group(1)))
                    for bp, bn, bt in body:
                        for p, a in zip(params, args):
                            bt = re.sub(r'(?<![\w@])' + re.escape(p) + r'(?!\w)', a, bt)
                        bt = bt.replace('@@', '@_m%d_' % counter[0])
                        res.append((path, n, bt))
                    changed = True
                else:
                    res.append((path, n, text))
            out = res
            if not changed:
                break
        return out

    # -- one pass
    def run_pass(self, final):
        self.pc = None
        self.out = {}  # address -> byte
        self.scope = ''
        self.exports = []
        self.asserts = []
        self.listing = []
        for idx, (path, n, text) in enumerate(self.lines):
            self.where = '%s:%d' % (os.path.basename(path), n)
            try:
                self.line(idx, text, final)
            except Undefined as e:
                if final:
                    raise AsmError('%s: undefined symbol %s' % (self.where, e))
            except AsmError as e:
                raise AsmError('%s: %s' % (self.where, e))

    def full(self, name):
        if name.startswith('@'):
            return self.scope + name
        return name

    def lookup(self, name):
        if name == '*':
            if self.pc is None:
                raise AsmError('no .org')
            return self.pc
        key = self.full(name)
        if key in self.syms:
            return self.syms[key]
        raise Undefined(key)

    def eval(self, s):
        return Expr(tokenize(s), self.lookup).parse()

    def try_eval(self, s):
        try:
            return self.eval(s)
        except Undefined:
            return None

    def define(self, name, value):
        key = self.full(name)
        if key in self.locked:          # -D override from the command line wins
            return
        old = self.pass_defs.get(key)
        if old is not None and old != value:
            raise AsmError('symbol %s redefined' % key)
        self.pass_defs[key] = value
        if self.syms.get(key) != value:
            self.changed = True
        self.syms[key] = value

    def emit(self, *bs):
        if self.pc is None:
            raise AsmError('code before .org')
        for b in bs:
            if self.pc in self.out:
                raise AsmError('overlapping output at $%04X' % self.pc)
            self.out[self.pc] = b & 0xFF
            self.pc += 1

    def line(self, idx, text, final):
        s = text.strip()
        if not s:
            return
        m = re.match(r'([@A-Za-z_][\w@]*)\s*:\s*(.*)$', s)
        if m:
            name = m.group(1)
            if not name.startswith('@'):
                self.scope = name
            self.define(name, self.pc)
            s = m.group(2).strip()
            if not s:
                return
        m = re.match(r'([A-Za-z_]\w*)\s*=\s*(.+)$', s)
        if m:
            v = self.try_eval(m.group(2))
            if v is None:
                if final:
                    self.eval(m.group(2))
                return
            self.define(m.group(1), v)
            return
        if s.startswith('.'):
            return self.directive(s, final)
        m = re.match(r'([A-Za-z]{3})\b\s*(.*)$', s)
        if not m or m.group(1).upper() not in OPCODES:
            raise AsmError('unknown instruction: %s' % s)
        self.instruction(idx, m.group(1).upper(), m.group(2).strip(), final)

    def directive(self, s, final):
        m = re.match(r'\.(\w+)\s*(.*)$', s)
        d, rest = m.group(1).lower(), m.group(2)
        if d == 'org':
            self.pc = self.eval(rest)
        elif d == 'byte' or d == 'db':
            for a in split_args(rest):
                if a.startswith('"'):
                    body = a[1:-1].encode('ascii')
                    self.emit(*body)
                else:
                    v = self.try_eval(a)
                    if v is None:
                        if final:
                            self.eval(a)
                        v = 0
                    if final and not (-128 <= v <= 255):
                        raise AsmError('byte out of range: %s = %d' % (a, v))
                    self.emit(v)
        elif d == 'word' or d == 'dw':
            for a in split_args(rest):
                v = self.try_eval(a)
                if v is None:
                    if final:
                        self.eval(a)
                    v = 0
                self.emit(v & 0xFF, (v >> 8) & 0xFF)
        elif d == 'res':
            args = split_args(rest)
            cnt = self.eval(args[0])
            val = self.eval(args[1]) if len(args) > 1 else self.fill
            self.emit(*([val] * cnt))
        elif d == 'align':
            args = split_args(rest)
            n = self.eval(args[0])
            val = self.eval(args[1]) if len(args) > 1 else self.fill
            while self.pc % n:
                self.emit(val)
        elif d == 'assert':
            args = split_args(rest)
            if final:
                if not self.eval(args[0]):
                    raise AsmError('assertion failed: %s %s' % (args[0], args[1] if len(args) > 1 else ''))
        elif d == 'delay':
            self.delay(self.eval(rest), final)
        elif d == 'export':
            for a in split_args(rest):
                self.exports.append(self.full(a))
        else:
            raise AsmError('unknown directive .%s' % d)

    def delay(self, n, final):
        """Busy-wait exactly n CPU cycles (n = 0 or >= 2). Clobbers X and the flags."""
        if n == 1 or n < 0:
            raise AsmError('.delay %d impossible (need 0 or >= 2)' % n)
        while n > 1281:  # one loop covers at most 5*256+1 cycles
            self.delay(1000, final)
            n -= 1000
        if n >= 12:
            k, r = (n - 1) // 5, (n - 1) % 5
            if r == 1:
                k, r = k - 1, 6
            self.emit(0xA2, k & 0xFF)             # ldx #k          2
            top = self.pc
            self.emit(0xCA, 0xD0, 0xFD)           # dex / bne top   5k-1
            if final and (top >> 8) != (self.pc >> 8):
                raise AsmError('.delay loop crosses a page boundary at $%04X' % top)
            n = r
        while n >= 2:
            if n == 3 or n == 5:
                self.emit(0x4C, (self.pc + 3) & 0xFF, (self.pc + 3) >> 8)  # jmp *+3    3
                n -= 3
            else:
                self.emit(0xEA)                   # nop             2
                n -= 2

    def instruction(self, idx, mn, op, final):
        modes = OPCODES[mn]
        force_abs = False
        if op.lower().startswith('a:'):
            force_abs, op = True, op[2:].strip()
        if op == '' or op.upper() == 'A':
            mode = 'acc' if 'acc' in modes else 'imp'
            if mode not in modes:
                raise AsmError('%s needs an operand' % mn)
            self.emit(modes[mode])
            return
        if 'rel' in modes:
            target = self.try_eval(op)
            self.emit(modes['rel'])
            if target is None:
                if final:
                    self.eval(op)
                self.emit(0)
                return
            off = target - (self.pc + 1)
            if final and not -128 <= off <= 127:
                raise AsmError('branch out of range (%d)' % off)
            self.emit(off & 0xFF)
            return
        expr, base = op, None
        if op.startswith('#'):
            base, expr = 'imm', op[1:]
        else:
            m = re.match(r'\((.*)\)\s*,\s*[yY]$', op)
            if m:
                base, expr = 'indy', m.group(1)
            else:
                m = re.match(r'\((.*),\s*[xX]\s*\)$', op)
                if m:
                    base, expr = 'indx', m.group(1)
                else:
                    m = re.match(r'\((.*)\)$', op)
                    if m and mn == 'JMP':
                        base, expr = 'ind', m.group(1)
                    else:
                        m = re.match(r'(.*),\s*([xXyY])$', op)
                        if m:
                            base, expr = 'idx' + m.group(2).lower(), m.group(1)
                        else:
                            base = 'mem'
        v = self.try_eval(expr)
        if v is None and final:
            self.eval(expr)
        # zero page only for operands known before this line on the first pass (sizes stay fixed)
        known_early = self.sizes.setdefault(idx, v is not None)
        if base == 'imm':
            if final and v is not None and not -128 <= v <= 255:
                raise AsmError('immediate out of range: %d' % v)
            self.emit(modes['imm'], 0 if v is None else v)
            return
        if base in ('indx', 'indy'):
            self.emit(modes[base], 0 if v is None else v)
            return
        if base == 'ind':
            v = 0 if v is None else v
            self.emit(modes['ind'], v & 0xFF, v >> 8)
            return
        zp_mode = {'mem': 'zp', 'idxx': 'zpx', 'idxy': 'zpy'}[base]
        abs_mode = {'mem': 'abs', 'idxx': 'absx', 'idxy': 'absy'}[base]
        use_zp = (not force_abs and known_early and v is not None and 0 <= v < 256 and zp_mode in modes)
        if use_zp:
            self.emit(modes[zp_mode], v)
            return
        if abs_mode not in modes:
            raise AsmError('%s does not support mode %s' % (mn, abs_mode))
        v = 0 if v is None else v
        self.emit(modes[abs_mode], v & 0xFF, (v >> 8) & 0xFF)

    def assemble(self, path, predefined=None, overrides=None):
        self.load(path)
        self.syms, self.sizes = dict(predefined or {}), {}
        self.syms.update(overrides or {})
        self.locked = set(overrides or {})
        for _ in range(20):
            self.changed, self.pass_defs = False, {}
            self.run_pass(final=False)
            if not self.changed:
                break
        self.pass_defs = {}
        self.run_pass(final=True)
        return self.out, self.syms, self.exports


def image(out, start, end, fill=0xFF):
    data = bytearray([fill] * (end - start))
    for a, b in out.items():
        if not start <= a < end:
            raise AsmError('byte outside image at $%04X' % a)
        data[a - start] = b
    return bytes(data)


if __name__ == '__main__':
    a = Assembler()
    out, syms, exports = a.assemble(sys.argv[1])
    lo, hi = min(out), max(out)
    print('assembled %d bytes, $%04X-$%04X' % (len(out), lo, hi))
