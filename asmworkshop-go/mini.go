// mini.go — встроенный минимальный движок x86 (страховка без nasm/objdump).
// Покрывает частые инструкции: mov/add/sub/cmp reg,imm16|32; push/pop; int;
// nop/ret/hlt/cli/sti/cld/std; короткие переходы je/jne/jmp rel8.
package main

import (
	"fmt"
	"strconv"
	"strings"
)

// ── регистры ──

var regs8 = map[string]byte{"al": 0, "cl": 1, "dl": 2, "bl": 3, "ah": 4, "ch": 5, "dh": 6, "bh": 7}
var regs16 = map[string]uint8{"ax": 0, "cx": 1, "dx": 2, "bx": 3, "sp": 4, "bp": 5, "si": 6, "di": 7}
var regs32 = map[string]uint8{"eax": 0, "ecx": 1, "edx": 2, "ebx": 3, "esp": 4, "ebp": 5, "esi": 6, "edi": 7}
var regs64 = map[string]uint8{"rax": 0, "rcx": 1, "rdx": 2, "rbx": 3, "rsp": 4, "rbp": 5, "rsi": 6, "rdi": 7,
	"r8": 8, "r9": 9, "r10": 10, "r11": 11, "r12": 12, "r13": 13, "r14": 14, "r15": 15}

func parseNum(s string) (int64, bool) {
	s = strings.TrimSpace(s)
	neg := false
	if strings.HasPrefix(s, "-") {
		neg, s = true, s[1:]
	}
	var v uint64
	var err error
	switch {
	case strings.HasPrefix(s, "0x") || strings.HasPrefix(s, "0X"):
		v, err = strconv.ParseUint(s[2:], 16, 64)
	case strings.HasSuffix(s, "h") || strings.HasSuffix(s, "H"):
		v, err = strconv.ParseUint(strings.TrimSuffix(strings.TrimSuffix(s, "h"), "H"), 16, 64)
	case strings.HasPrefix(s, "'"):
		if len(s) >= 3 && s[len(s)-1] == '\'' {
			v = uint64(s[1])
		} else {
			err = fmt.Errorf("bad char")
		}
	default:
		v, err = strconv.ParseUint(s, 10, 64)
	}
	if err != nil {
		return 0, false
	}
	if neg {
		return -int64(v), true
	}
	return int64(v), true
}

func splitOps(s string) []string {
	var out []string
	depth := 0
	last := 0
	for i := 0; i < len(s); i++ {
		switch s[i] {
		case '[', '(':
			depth++
		case ']', ')':
			depth--
		case ',':
			if depth == 0 {
				out = append(out, strings.TrimSpace(s[last:i]))
				last = i + 1
			}
		}
	}
	if t := strings.TrimSpace(s[last:]); t != "" || last < len(s) {
		out = append(out, strings.TrimSpace(s[last:]))
	}
	return out
}

// miniAssemble собирает ОДНУ базовую инструкцию.
func miniAssemble(line string, m Mode) ([]byte, error) {
	line = strings.TrimSpace(line)
	if c := strings.IndexByte(line, ';'); c >= 0 {
		line = line[:c]
	}
	if line == "" {
		return nil, fmt.Errorf("пустая строка")
	}
	// метка в начале строки — отбросить
	if idx := strings.IndexByte(line, ':'); idx > 0 && !strings.ContainsAny(line[:idx], " ,") {
		line = strings.TrimSpace(line[idx+1:])
	}
	f := strings.Fields(line)
	op := strings.ToLower(f[0])
	ops := splitOps(strings.TrimSpace(strings.TrimPrefix(line, f[0])))

	// директивы db
	if op == "db" {
		var bs []byte
		for _, t := range ops {
			if v, ok := parseNum(t); ok {
				bs = append(bs, byte(v))
				continue
			}
			if strings.HasPrefix(t, "\"") {
				s := strings.Trim(t, "\"")
				for i := 0; i < len(s); i++ {
					bs = append(bs, s[i])
				}
				continue
			}
			return nil, fmt.Errorf("не понимаю %q", t)
		}
		return bs, nil
	}

	oneArg := map[string][]byte{
		"nop": {0x90},
		"ret": {0xC3},
		"hlt": {0xF4},
		"cli": {0xFA},
		"sti": {0xFB},
		"cld": {0xFC},
		"std": {0xFD},
		"cbw": {0x98},
		"cwd": {0x99},
	}
	alts := map[string]string{"jz": "je", "jnz": "jne"}
	if a, ok := alts[op]; ok {
		op = a
	}

	operandSize := 2 // байт (16/32/64-bit режим)
	switch m {
	case Mode32:
		operandSize = 4
	case Mode64:
		operandSize = 8
	}

	// inc/dec reg — однобайтные формы 0x40+р / 0x48+р (в 32/64-бит требует префикса 0xFF)
	if op == "inc" || op == "dec" {
		if len(ops) == 1 {
			r := strings.ToLower(ops[0])
			if operandSize >= 4 {
				if v, ok := regs32[r]; ok {
					b := []byte{0xFF}
					if op == "dec" {
						b[0] = 0xFE
					}
					return append(b, 0xC0+v), nil
				}
				if v, ok := regs64[r]; ok {
					base := byte(0xFF)
					if op == "dec" {
						base = 0xFE
					}
					if v >= 8 {
						return []byte{0x40 | (v & 8), base, 0xC0 + v&7}, nil
					}
					return []byte{base, 0xC0 + v}, nil
				}
			}
			if v, ok := regs16[r]; ok {
				b := []byte{}
				if operandSize != 2 {
					b = append(b, 0x66)
				}
				base := byte(0x40)
				if op == "dec" {
					base = 0x48
				}
				return append(b, base+v), nil
			}
			if v, ok := regs8[r]; ok && !ahchdhbh(r) {
				// FE /0 inc r/m8, FE /1 dec r/m8
				reg := byte(0)
				switch r {
				case "al":
					reg = 0
				case "cl":
					reg = 1
				case "dl":
					reg = 2
				case "bl":
					reg = 3
				default:
					return nil, fmt.Errorf("inc/dec %s без nasm не поддерживается", r)
				}
				_ = v
				modrm := byte(0xC0 | reg)
				if op == "dec" {
					modrm |= 1 << 3
				}
				return []byte{0xFE, modrm}, nil
			}
		}
	}

	switch op {
	case "nop", "ret", "hlt", "cli", "sti", "cld", "std", "cbw", "cwd":
		return oneArg[op], nil
	case "int":
		if len(ops) == 1 {
			if v, ok := parseNum(ops[0]); ok {
				return []byte{0xCD, byte(v)}, nil
			}
		}
	case "jmp", "je", "jne":
		if len(ops) == 1 {
			// только числовая смещённая форма для мини-движка: short label невозможна без карты
			return nil, fmt.Errorf("переход к метке требует nasm")
		}
	case "push", "pop":
		if len(ops) == 1 {
			r := strings.ToLower(ops[0])
			if operandSize >= 4 {
				if v, ok := regs32[r]; ok {
					if operandSize == 8 {
						return []byte{0x50 + v}, nil
					}
					return []byte{0x50 + v}, nil
				}
				if v, ok := regs64[r]; ok {
					if v >= 8 {
						return []byte{0x41, 0x50 + v - 8}, nil
					}
					return []byte{0x50 + v}, nil
				}
			}
			if v, ok := regs16[r]; ok {
				b := []byte{}
				if operandSize == 2 {
					b = append(b, 0x50+v)
				} else {
					b = append(b, 0x66, 0x50+v)
				}
				return b, nil
			}
			if operandSize >= 4 {
				if v, ok := parseNum(r); ok {
					if int64(int8(v)) == v {
						return []byte{0x6A, byte(v)}, nil
					}
					b := []byte{0x68}
					for i := 0; i < 4; i++ {
						b = append(b, byte(v>>(8*i)))
					}
					return b, nil
				}
			} else if operandSize == 2 {
				if v, ok := parseNum(r); ok {
					b := []byte{0x68}
					b = append(b, byte(v), byte(v>>8))
					return b, nil
				}
			}
		}
	case "mov", "add", "sub", "cmp", "and", "or", "xor", "test", "adc", "sbb":
		if len(ops) != 2 {
			break
		}
		dst, src := strings.ToLower(ops[0]), strings.ToLower(ops[1])
		alu := map[string]uint8{"add": 0, "or": 1, "adc": 2, "sbb": 3, "and": 4, "sub": 5, "xor": 6, "cmp": 7}
		code, isAlu := alu[op]
		if !isAlu {
			break
		}
		// reg, imm
		if sz, rnum, rex, ok := lookupReg(dst, m); ok {
			if v, numOK := parseNum(src); numOK && op != "test" {
				return encodeRegImm(op, code, sz, rnum, rex, v, m), nil
			}
			// reg, reg
			if sz2, rnum2, rex2, ok2 := lookupReg(src, m); ok2 && sz2 == sz {
				return encodeRegReg(code, sz, rnum, rex, rnum2, rex2), nil
			}
		}
		// mem <-> reg: mov/add/sub/cmp [bx+si], ax и т.п.
		if dmem, ok := parseMemOperand(dst); ok {
			if sz, rnum, _, ok2 := lookupReg(src, m); ok2 {
				return encodeMemOp(code, op == "mov", sz, byte(rnum), dmem, m)
			}
			if v, ok3 := parseNum(src); ok3 && op == "mov" {
				sz := defaultSize(m, false, false)
				if sz == 1 {
					sz = 2
				}
				return encodeMemImm(sz, dmem, v, m)
			}
		}
		if smem, ok := parseMemOperand(src); ok {
			if sz, rnum, _, ok2 := lookupReg(dst, m); ok2 {
				return encodeMemOp(code, true, sz, byte(rnum), smem, m)
			}
		}
		// r/m special: mov al, imm8 / ax, imm16 handled above via lookupReg
	}
	return nil, fmt.Errorf("не поддерживаю «%s» без установленного nasm", line)
}

// lookupReg: размер операнда в байтах (1/2/4/8), номер, bit REX, ок.
func lookupReg(name string, m Mode) (int, uint8, bool, bool) {
	if v, ok := regs8[name]; ok {
		if ahchdhbh(name) {
			return 1, v, false, true
		}
		return 1, v, false, true
	}
	switch m {
	case ModeCOM, Mode16:
		if v, ok := regs16[name]; ok {
			return 2, v, false, true
		}
	case Mode32:
		if v, ok := regs32[name]; ok {
			return 4, v, false, true
		}
	case Mode64:
		if v, ok := regs64[name]; ok {
			rex := v >= 8
			return 8, v & 7, rex, true
		}
		if v, ok := regs32[name]; ok { // eax и т.д. в 64-бит — через 0x66? упрощённо берём 32-бит
			return 4, v, false, true
		}
	}
	// кросс-размерные: 16-бит рег в 32-бит режиме (0x66 префикс) и наоборот
	if v, ok := regs16[name]; ok {
		return 2, v, false, true
	}
	if v, ok := regs32[name]; ok {
		return 4, v, false, true
	}
	return 0, 0, false, false
}

func ahchdhbh(name string) bool {
	return name == "ah" || name == "ch" || name == "dh" || name == "bh"
}

func encodeRegImm(op string, code uint8, sz int, rnum uint8, rexW bool, v int64, m Mode) []byte {
	var b []byte
	// mov reg, imm — особая форма B0+r/B8+r
	if op == "mov" {
		switch sz {
		case 1:
			if needsHighREX(rnum, rexW) {
				b = append(b, 0x40)
			}
			return append(b, 0xB0+rnum, byte(v))
		case 2:
			if m == Mode32 || m == Mode64 {
				b = append(b, 0x66)
			}
			b = append(b, 0xB8+rnum)
			return append(b, byte(v), byte(v>>8))
		case 4:
			if rexW {
				b = append(b, 0x48)
			}
			b = append(b, 0xB8+rnum)
			for i := 0; i < 4; i++ {
				b = append(b, byte(v>>(8*i)))
			}
			return b
		case 8:
			b = append(b, 0x48, 0xB8+rnum)
			for i := 0; i < 8; i++ {
				b = append(b, byte(v>>(8*i)))
			}
			return b
		}
	}
	// add/and/sub/xor/cmp/or reg, imm
	if sz == 1 {
		if needsHighREX(rnum, rexW) {
			b = append(b, 0x40)
		}
		b = append(b, 0x80+code<<3, 0xC0+rnum, byte(v))
		return b
	}
	if sz == 2 && (m == Mode32 || m == Mode64) {
		b = append(b, 0x66)
	}
	if sz == 8 {
		b = append(b, 0x48)
	}
	if int64(int8(v)) == v && op != "test" {
		b = append(b, 0x83+code<<3, 0xC0+rnum, byte(v))
		return b
	}
	b = append(b, 0x81+code<<3, 0xC0+rnum)
	n := sz
	if n == 8 {
		n = 4
	}
	for i := 0; i < n; i++ {
		b = append(b, byte(v>>(8*i)))
	}
	return b
}

func needsHighREX(rnum uint8, rexW bool) bool { return false }

func encodeRegReg(code uint8, sz int, dst uint8, dstREX bool, src uint8, srcREX bool) []byte {
	var b []byte
	modrm := byte(0xC0) | dst<<3 | src
	if sz == 1 {
		if dstREX || srcREX {
			b = append(b, 0x40)
		}
		return append(b, 0x00+code<<3, modrm)
	}
	if sz == 2 {
		b = append(b, 0x66)
	}
	rexR := boolByte(srcREX)<<2 | boolByte(dstREX)
	var opcodeTail byte = 0x01
	if sz == 8 {
		rexR |= 8
	}
	if sz == 4 || sz == 8 {
		// в 32/64 режимах префикс 0x66 не нужен; rex добавляем если есть
		if rexR != 0 {
			b = append(b, 0x40|rexR)
		}
		return append(b, opcodeTail+code<<3, modrm)
	}
	return append(b, opcodeTail+code<<3, modrm)
}

func boolByte(v bool) uint8 {
	if v {
		return 1
	}
	return 0
}

// ─────────────── память: [bx+si], [bp+4], [eax+8] и т.п. ───────────────

type memOp struct {
	form  string // "m16" — 16-битная форма (base/index из bx,bp,si,di), "m32" — с SIB
	base  string
	index string
	disp  int64
	size  int // явный размер из byte/word/dword ptr, 0 = нет
}

var rm16 = map[string]uint8{"bx": 3, "bp": 5, "si": 6, "di": 7}

func parseMemOperand(tok string) (memOp, bool) {
	t := strings.ToLower(strings.TrimSpace(tok))
	var mo memOp
	for _, p := range []struct {
		n string
		s int
	}{{"byte ", 1}, {"word ", 2}, {"dword ", 4}, {"qword ", 8}} {
		if strings.HasPrefix(t, p.n) {
			t = strings.TrimSpace(t[len(p.n):])
			mo.size = p.s
		}
	}
	if strings.HasPrefix(t, "ptr ") {
		t = strings.TrimSpace(t[4:])
	}
	open := strings.IndexByte(t, '[')
	if open < 0 {
		return mo, false
	}
	if open > 0 {
		seg := strings.TrimSuffix(t[:open], ":")
		if seg != "es" && seg != "ds" && seg != "ss" && seg != "fs" && seg != "gs" {
			return mo, false
		}
	}
	closeIdx := strings.LastIndexByte(t, ']')
	inner := strings.TrimSpace(t[open+1 : closeIdx])
	if inner == "" {
		return mo, false
	}
	parts := strings.Split(inner, "+")
	first := strings.TrimSpace(parts[0])
	// чистое число: [1234h] — 16-бит abs (mod=00 rm=6)
	if _, _, _, ok := lookupReg(first, Mode16); !ok {
		if len(parts) == 1 {
			if v, ok2 := parseNum(first); ok2 {
				mo.form = "m16abs"
				mo.disp = v
				return mo, true
			}
		}
		return mo, false
	}
	var disp int64
	for _, rp := range parts[1:] {
		rp = strings.TrimSpace(rp)
		if _, _, _, ok := lookupReg(rp, Mode16); ok && isLow16(rp) {
			if mo.index != "" {
				return mo, false
			}
			mo.index = rp
			continue
		}
		v, ok := parseNum(rp)
		if !ok {
			return mo, false
		}
		disp += v
	}
	mo.disp = disp
	if isLow16(first) {
		if first != "bx" && first != "bp" && first != "si" && first != "di" {
			return mo, false
		}
		if mo.index != "" && mo.index != "si" && mo.index != "di" {
			return mo, false
		}
		mo.form = "m16"
		mo.base = first
		return mo, true
	}
	// 32/64-бит база
	mo.form = "m32"
	mo.base = first
	return mo, true
}

func isLow16(r string) bool {
	_, ok := regs16[r]
	return ok
}

// encodeMemOp: dir=true → опкод /r с reg в reg-поле (mov/add reg, [mem]);
// dir=false → mov/add [mem], reg.
func encodeMemOp(code uint8, isMov bool, sz int, reg byte, mo memOp, m Mode) ([]byte, error) {
	var b []byte
	defSz := defaultSize(m, false, false)
	if sz == 2 && defSz != 2 {
		b = append(b, 0x66)
	} else if sz == 4 && defSz == 2 {
		b = append(b, 0x66)
	}
	var opcode byte
	if sz == 1 {
		opcode = code<<3 + 0x00
		if isMov {
			opcode = 0x8A
		}
	} else {
		opcode = code<<3 + 0x01
		if isMov {
			opcode = 0x8B
		}
	}
	rm, err := encodeModRM1632(mo, reg, m)
	if err != nil {
		return nil, err
	}
	return append(append(b, opcode), rm...), nil
}

func encodeMemImm(sz int, mo memOp, v int64, m Mode) ([]byte, error) {
	var b []byte
	defSz := defaultSize(m, false, false)
	if sz == 2 && defSz != 2 {
		b = append(b, 0x66)
	} else if sz == 4 && defSz == 2 {
		b = append(b, 0x66)
	}
	opcode := byte(0xC7)
	if sz == 1 {
		opcode = 0xC6
	}
	rm, err := encodeModRM1632(mo, 0, m)
	if err != nil {
		return nil, err
	}
	b = append(append(b, opcode), rm...)
	n := sz
	if n >= 8 {
		n = 4
	}
	for i := 0; i < n; i++ {
		b = append(b, byte(v>>(8*i)))
	}
	return b, nil
}

func encodeModRM1632(mo memOp, reg byte, m Mode) ([]byte, error) {
	switch mo.form {
	case "m16abs":
		return []byte{0x06 | reg<<3, byte(mo.disp), byte(mo.disp >> 8)}, nil
	case "m16":
		rm, mod, err := modrm16(mo.base, mo.index, mo.disp)
		if err != nil {
			return nil, err
		}
		out := []byte{mod<<6 | reg<<3 | rm}
		if mod == 1 {
			out = append(out, byte(int8(mo.disp)))
		} else if mod == 2 {
			out = append(out, byte(mo.disp), byte(mo.disp>>8))
		}
		return out, nil
	case "m32":
		numOf := func(n string) (uint8, bool) {
			if v, ok := regs32[n]; ok {
				return v, true
			}
			if v, ok := regs64[n]; ok {
				return v & 7, true
			}
			return 0, false
		}
		bn, ok := numOf(mo.base)
		if !ok || bn == 4 || (bn == 5 && mo.index == "") {
			if bn == 5 && mo.index == "" {
				// [rbp]/[ebp] требует disp8=0
				mod := byte(1)
				d := mo.disp
				if d >= -128 && d <= 127 {
					return []byte{mod<<6 | reg<<3 | 5, byte(int8(d))}, nil
				}
				return []byte{2<<6 | reg<<3 | 5, byte(d), byte(d >> 8), byte(d >> 16), byte(d >> 24)}, nil
			}
			return nil, fmt.Errorf("база %s недоступна", mo.base)
		}
		if mo.index != "" {
			in, ok2 := numOf(mo.index)
			if !ok2 {
				return nil, fmt.Errorf("индекс %s недоступен", mo.index)
			}
			sib := in<<3 | bn
			mod := byte(0)
			if mo.disp != 0 {
				if mo.disp >= -128 && mo.disp <= 127 {
					mod = 1
				} else {
					mod = 2
				}
			}
			out := []byte{mod<<6 | reg<<3 | 4, sib}
			if mod == 1 {
				out = append(out, byte(int8(mo.disp)))
			} else if mod == 2 {
				for i := 0; i < 4; i++ {
					out = append(out, byte(mo.disp>>(8*i)))
				}
			}
			return out, nil
		}
		mod := byte(0)
		if mo.disp != 0 {
			if mo.disp >= -128 && mo.disp <= 127 {
				mod = 1
			} else {
				mod = 2
			}
		}
		out := []byte{mod<<6 | reg<<3 | bn}
		if mod == 1 {
			out = append(out, byte(int8(mo.disp)))
		} else if mod == 2 {
			for i := 0; i < 4; i++ {
				out = append(out, byte(mo.disp>>(8*i)))
			}
		}
		return out, nil
	}
	return nil, fmt.Errorf("форма памяти не распознана")
}

func modrm16(base, index string, disp int64) (rm uint8, mod uint8, err error) {
	switch {
	case base == "bx" && index == "si":
		rm = 0
	case base == "bx" && index == "di":
		rm = 1
	case base == "bp" && index == "si":
		rm = 2
	case base == "bp" && index == "di":
		rm = 3
	case base == "si" && index == "":
		rm = 6
	case base == "di" && index == "":
		rm = 5
	case base == "bp" && index == "":
		rm = 4
	case base == "bx" && index == "":
		rm = 7
	default:
		return 0, 0, fmt.Errorf("недопустимая пара [%s+%s]", base, index)
	}
	if disp != 0 || rm == 6 {
		if disp >= -128 && disp <= 127 {
			mod = 1
		} else {
			mod = 2
		}
	}
	return rm, mod, nil
}

// ─────────────── мини-деассемблер ───────────────

var regNames8 = [8]string{"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"}
var regNames16 = [8]string{"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"}
var regNames32 = [8]string{"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"}
var regNames64 = [8]string{"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"}
var aluNames = [8]string{"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"}

func regSet(m Mode, sz int) [8]string {
	switch {
	case sz == 1:
		return regNames8
	case sz == 2 || m == ModeCOM || m == Mode16:
		return regNames16
	case sz == 4 || m == Mode32:
		return regNames32
	default:
		return regNames64
	}
}

// miniDisasm разбирает одну инструкцию, возвращает текст и длину в байтах.
func miniDisasm(d []byte, m Mode) (string, int) {
	if len(d) == 0 {
		return "", 0
	}
	i := 0
	var prefix66, rexW bool
	for i < len(d) {
		if d[i] == 0x66 && (m == Mode32 || m == Mode64) && !prefix66 {
			prefix66 = true
			i++
			continue
		}
		if m == Mode64 && d[i]&0xF0 == 0x40 && !rexW {
			rexW = d[i]&8 != 0
			i++
			continue
		}
		break
	}
	sz := defaultSize(m, prefix66, rexW)
	names := regSet(m, sz)
	if rexW {
		names = regNames64
		sz = 8
	}
	if i >= len(d) {
		return "", 0
	}
	op := d[i]
	next := func(n int) (int64, bool) {
		if i+n >= len(d) {
			return 0, false
		}
		var v uint64
		for k := 0; k < n; k++ {
			v |= uint64(d[i+1+k]) << (8 * k)
		}
		i += 1 + n
		return signExtend(v, n), true
	}
	switch {
	case op&0xF8 == 0x88: // op r/m8, r8
		if i+1 >= len(d) {
			return "", 0
		}
		mod := d[i+1]
		if mod&0xC0 == 0xC0 {
			i += 2
			return fmt.Sprintf("%s %s, %s", aluNames[op>>3], names[mod&7], names[(mod>>3)&7]), i
		}
	case op&0xF8 == 0x80: // op r/m8, imm8
		if mod := d[i+1]; mod&0xC0 == 0xC0 {
			name := aluNames[(op>>3)&7]
			if name == "mov" {
				name = "mov"
			}
			v, ok := next(1)
			if !ok {
				return "", 0
			}
			return fmt.Sprintf("%s %s, 0x%X", name, names[d[i+1]&7], v), i
		}
	case op&0xF8 == 0x89: // op r/m, r (16/32/64)
		if i+1 < len(d) && d[i+1]&0xC0 == 0xC0 {
			mod := d[i+1]
			i += 2
			return fmt.Sprintf("%s %s, %s", aluNames[op>>3], names[mod&7], names[(mod>>3)&7]), i
		}
	case op&0xF8 == 0x81: // op r/m, imm32/16
		if i+1 < len(d) && d[i+1]&0xC0 == 0xC0 {
			rm := d[i+1] & 7
			n := sz
			if n == 8 {
				n = 4
			}
			v, ok := next(n)
			if !ok {
				return "", 0
			}
			return fmt.Sprintf("%s %s, 0x%X", aluNames[op>>3], names[rm], v), i
		}
	case op&0xF8 == 0x83: // op r/m, imm8
		if i+1 < len(d) && d[i+1]&0xC0 == 0xC0 {
			rm := d[i+1] & 7
			v, ok := next(1)
			if !ok {
				return "", 0
			}
			return fmt.Sprintf("%s %s, 0x%X", aluNames[op>>3], names[rm], v&0xFF), i
		}
	case op&0xF8 == 0xB0: // mov r8/r, imm
		rm := op & 7
		if sz == 1 {
			v, ok := next(1)
			if !ok {
				return "", 0
			}
			return fmt.Sprintf("mov %s, 0x%X", names[rm], v), i
		}
		n := sz
		if n == 8 {
			n = 4
		}
		v, ok := next(n)
		if !ok {
			return "", 0
		}
		return fmt.Sprintf("mov %s, 0x%X", names[rm], v), i
	case op&0xF8 == 0x50: // push r
		i++
		return fmt.Sprintf("push %s", names[op&7]), i
	case op&0xF8 == 0x58: // pop r
		i++
		return fmt.Sprintf("pop %s", names[op&7]), i
	case op == 0x68: // push imm16/32
		n := sz
		if n == 8 {
			n = 4
		}
		if n == 1 {
			n = 2
		}
		v, ok := next(n)
		if !ok {
			return "", 0
		}
		return fmt.Sprintf("push 0x%X", v), i
	case op == 0x6A: // push imm8
		v, ok := next(1)
		if !ok {
			return "", 0
		}
		return fmt.Sprintf("push 0x%X", v&0xFF), i
	case op == 0xCD: // int imm8
		v, ok := next(1)
		if !ok {
			return "", 0
		}
		return fmt.Sprintf("int 0x%X", v), i
	case op == 0x90:
		i++
		return "nop", i
	case op == 0xC3:
		i++
		return "ret", i
	case op == 0xF4:
		i++
		return "hlt", i
	case op == 0xEB: // jmp rel8
		v, ok := next(1)
		if !ok {
			return "", 0
		}
		return fmt.Sprintf("jmp 0x%X", v), i
	case op == 0x74 || op == 0x75: // je/jne rel8
		v, ok := next(1)
		if !ok {
			return "", 0
		}
		name := "je"
		if op == 0x75 {
			name = "jne"
		}
		return fmt.Sprintf("%s 0x%X", name, v), i
	}
	return "", 0
}

func defaultSize(m Mode, prefix66, rexW bool) int {
	switch m {
	case ModeCOM, Mode16:
		if prefix66 {
			return 4
		}
		return 2
	case Mode32:
		if prefix66 {
			return 2
		}
		return 4
	case Mode64:
		if rexW {
			return 8
		}
		if prefix66 {
			return 2
		}
		return 4
	}
	return 2
}

func signExtend(v uint64, n int) int64 {
	switch n {
	case 1:
		return int64(int8(v))
	case 2:
		return int64(int16(v))
	case 4:
		return int64(int32(v))
	}
	return int64(v)
}
