// mini2_darwin.go — расширение встроенного движка для macOS-сборки:
// полная поддержка 16-битных форм mov reg,imm (B8+r / C7 / 81 / 83 с префиксом 0x66),
// инкремент/декремент регистров. Используется gui_darwin.go напрямую, чтобы
// приложение на Mac работало без nasm/objdump.
//go:build darwin && amd64

package main

import "fmt"

// mini2Assemble — сборка одной инструкции (расширенный мини-движок).
func mini2Assemble(line string, m Mode) ([]byte, error) {
	if b, err := miniAssemble(line, m); err == nil {
		return b, nil
	}
	f := splitOpsTrim(line)
	if len(f) == 0 {
		return nil, fmt.Errorf("пустая строка")
	}
	op := f[0]
	var ops []string
	if len(f) > 1 {
		ops = splitOps(f[1])
	}
	switch op {
	case "mov":
		if len(ops) == 2 {
			if sz, rn, rex, ok := lookupReg(ops[0], m); ok && sz == 2 {
				if v, ok := parseNum(ops[1]); ok {
					b := []byte{}
					if m == Mode32 || m == Mode64 {
						b = append(b, 0x66)
					}
					if int64(int8(v)) == v && false {
						_ = v
					}
					b = append(b, 0xB8+rn, byte(v), byte(v>>8))
					_ = rex
					return b, nil
				}
			}
		}
	case "inc", "dec":
		if len(ops) == 1 {
			rn := map[string]int{"ax": 0, "cx": 1, "dx": 2, "bx": 3, "sp": 4, "bp": 5, "si": 6, "di": 7,
				"eax": 0, "ecx": 1, "edx": 2, "ebx": 3, "esi": 6, "edi": 7}
			if v, ok := rn[ops[0]]; ok {
				b := []byte{}
				if m == ModeCOM || m == Mode16 {
					b = append(b, 0x66)
				}
				base := byte(0x40)
				if op == "dec" {
					base = 0x48
				}
				return append(b, base+byte(v)), nil
			}
		}
	}
	return nil, fmt.Errorf("не поддерживаю «%s»", line)
}

// mini2Disasm — разбор одной инструкции (расширенный мини-движок).
func mini2Disasm(d []byte, m Mode) (string, int) {
	if s, n := miniDisasm(d, m); n > 0 {
		return s, n
	}
	i := 0
	var p66 bool
	for i < len(d) {
		if d[i] == 0x66 && (m == Mode32 || m == Mode64) && !p66 {
			p66 = true
			i++
			continue
		}
		break
	}
	if i >= len(d) {
		return "", 0
	}
	op := d[i]
	names := func() [8]string {
		if p66 {
			return regNames16
		}
		switch m {
		case ModeCOM, Mode16:
			return regNames16
		case Mode32, Mode64:
			return regNames32
		}
		return regNames16
	}()
	sz := 4
	if p66 {
		sz = 2
	} else if m == ModeCOM || m == Mode16 {
		sz = 2
	}
	read := func(n int) (int64, bool) {
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
	if op&0xF8 == 0xB8 { // mov r16/r32, imm16/32
		n := sz
		if n == 8 {
			n = 4
		}
		v, ok := read(n)
		if !ok {
			return "", 0
		}
		return fmt.Sprintf("mov %s, 0x%X", names[op&7], v), i
	}
	if op == 0xC7 || op == 0x81 || op == 0x83 { // mov/add/cmp r/m, imm
		if i+1 >= len(d) || d[i+1]&0xC0 != 0xC0 {
			return "", 0
		}
		rm := d[i+1] & 7
		var n int
		switch op {
		case 0x83:
			n = 1
		case 0xC7:
			n = sz
			if n == 8 {
				n = 4
			}
		case 0x81:
			n = sz
			if n == 8 {
				n = 4
			}
		}
		v, ok := read(n)
		if !ok {
			return "", 0
		}
		name := "mov"
		if op == 0x81 || op == 0x83 {
			name = aluNames[(d[i+1]>>3)&7]
		}
		return fmt.Sprintf("%s %s, 0x%X", name, names[rm], v), i
	}
	if op&0xF8 >= 0x40 && op&0xF8 <= 0x4F { // inc/dec r16 (в 32-бит — rex, пропускаем)
		if m == ModeCOM || m == Mode16 || p66 {
			base := op & 7
			name := "inc"
			code := (op >> 3) & 1
			if code == 1 {
				name = "dec"
			}
			i++
			return fmt.Sprintf("%s %s", name, names[base]), i
		}
	}
	return "", 0
}

func splitOpsTrim(line string) []string {
	line = trimLine(line)
	idx := -1
	for i := 0; i < len(line); i++ {
		if line[i] == ' ' || line[i] == '\t' {
			idx = i
			break
		}
	}
	if idx < 0 {
		return []string{line}
	}
	return []string{line[:idx], line[idx+1:]}
}

func trimLine(s string) string {
	for len(s) > 0 && (s[0] == ' ' || s[0] == '\t') {
		s = s[1:]
	}
	if c := indexOfByte(s, ';'); c >= 0 {
		s = s[:c]
	}
	for len(s) > 0 && (s[len(s)-1] == ' ' || s[len(s)-1] == '\t') {
		s = s[:len(s)-1]
	}
	return s
}

func indexOfByte(s string, b byte) int {
	for i := 0; i < len(s); i++ {
		if s[i] == b {
			return i
		}
	}
	return -1
}
