// engine.go — движок ASM Workshop (Go-версия, кроссплатформенная).
// Ассемблирование через NASM, деассемблирование через objdump (GNU binutils).
// Если внешних утилит нет — используется встроенный минимальный движок x86.
package main

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

// Mode — режим сборки/разбора.
type Mode int

const (
	ModeCOM Mode = iota // 16-bit DOS .COM (ORG 0x100)
	Mode16              // 16-bit raw
	Mode32              // 32-bit raw
	Mode64              // 64-bit raw
)

var modeBits = []string{"16", "16", "32", "64"}
var modeArch = []string{"i8086", "i8086", "i386", "i386:x86-64"}
var modeName = []string{"COM 16-bit (DOSBox)", "16-bit", "32-bit", "64-bit"}

// Result — ответ движка интерфейсу.
type Result struct {
	Text  string // сообщение статуса/ошибки
	IsErr bool
}

// workDir — временной каталог для файлов nasm/objdump.
func workDir() string {
	d := filepath.Join(os.TempDir(), "asmworkshop")
	os.MkdirAll(d, 0o777)
	return d
}

// lookTool ищет утилиту: сначала рядом с самим asmworkshop.exe,
// затем в PATH (exec.LookPath на Windows сам добавит расширение .exe).
func lookTool(name string) (string, error) {
	if exeDir, err := os.Executable(); err == nil {
		cand := filepath.Join(exeDir, name)
		if _, serr := os.Stat(cand); serr == nil {
			return cand, nil
		}
		if _, serr := os.Stat(cand + ".exe"); serr == nil {
			return cand + ".exe", nil
		}
	}
	return exec.LookPath(name)
}

func haveTool(name string) bool {
	_, err := lookTool(name)
	return err == nil
}

// mustTool — обёртка над lookTool; при отсутствии возвращает исходное имя,
// чтобы exec.Command дал привычную ошибку «executable not found».
func mustTool(name string) (string, error) {
	if p, err := lookTool(name); err == nil {
		return p, nil
	}
	return name, nil
}

// ─────────────────────────── ассемблирование ───────────────────────────

// Assemble: src (NASM-синтаксис, одна или несколько строк) → hex-строка байт.
func Assemble(src string, m Mode) (hex string, res Result) {
	src = strings.TrimRight(src, "\n")
	full := fmt.Sprintf("BITS %s\n", modeBits[m])
	if m == ModeCOM {
		full += "ORG 0x100\n"
	}
	full += "SECTION .text\n" + src + "\n"

	if haveTool("nasm") {
		dir := workDir()
		srcPath := filepath.Join(dir, fmt.Sprintf("in_%d.asm", os.Getpid()))
		binPath := filepath.Join(dir, fmt.Sprintf("out_%d.bin", os.Getpid()))
		defer os.Remove(srcPath)
		defer os.Remove(binPath)
		if err := os.WriteFile(srcPath, []byte(full), 0o666); err != nil {
			return "", Result{Text: "Не могу записать временный файл: " + err.Error(), IsErr: true}
		}
		toolNasm, _ := mustTool("nasm")
		cmd := exec.Command(toolNasm, "-f", "bin", "-O0", "-o", binPath, srcPath)
		var stderr bytes.Buffer
		cmd.Stderr = &stderr
		if err := cmd.Run(); err != nil {
			msg := firstLine(stderr.String())
			if msg == "" {
				msg = "nasm завершился с ошибкой"
			}
			return "", Result{Text: "NASM: " + msg, IsErr: true}
		}
		data, err := os.ReadFile(binPath)
		if err != nil {
			return "", Result{Text: "Нет файла результата NASM", IsErr: true}
		}
		if len(data) == 0 {
			return "", Result{Text: "Пустой код (только метки/директивы?)", IsErr: true}
		}
		hex = bytesToHex(data)
		expl := ExplainLine(src)
		txt := fmt.Sprintf("%d байт · режим %s", len(data), modeName[m])
		if expl != "" {
			txt += "\n" + expl
		}
		return hex, Result{Text: txt}
	}

	// Без NASM — встроенный мини-ассемблер (одна базовая инструкция).
	ins := lastInsnLine(src)
	data, err := miniAssemble(ins, m)
	if err != nil {
		return "", Result{Text: "Мини-движок: " + err.Error() +
			"\n(установите nasm для полного синтаксиса)", IsErr: true}
	}
	hex = bytesToHex(data)
	expl := ExplainLine(ins)
	txt := fmt.Sprintf("%d байт · режим %s · встроенный мини-движок", len(data), modeName[m])
	if expl != "" {
		txt += "\n" + expl
	}
	return hex, Result{Text: txt}
}

func bytesToHex(d []byte) string {
	parts := make([]string, len(d))
	for i, b := range d {
		parts[i] = fmt.Sprintf("%02X", b)
	}
	return strings.Join(parts, " ")
}

func firstLine(s string) string {
	s = strings.TrimSpace(s)
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		s = s[:i]
	}
	return s
}

// ─────────────────────────── деассемблирование ───────────────────────────

// Disassemble: hex-строка ("B8 01 00") → список инструкций по одной на строку.
func Disassemble(hexstr string, m Mode) (asm string, res Result) {
	data, err := parseHex(hexstr)
	if err != nil {
		return "", Result{Text: "Не нашёл ни одного байта в hex-строке", IsErr: true}
	}

	if haveTool("objdump") {
		out, rerr := objdumpDisasm(data, m)
		if rerr.IsErr {
			return "", rerr
		}
		lines := strings.Split(out, "\n")
		count := len(lines)
		expl := ExplainLine(lines[count-1])
		txt := fmt.Sprintf("%s · режим %s", pluralInsns(count), modeName[m])
		if expl != "" {
			txt += "\n" + expl
		}
		return out, Result{Text: txt}
	}

	// Встроенный мини-деасемблер.
	var lines []string
	for i := 0; i < len(data); {
		s, n := miniDisasm(data[i:], m)
		if n <= 0 {
			lines = append(lines, fmt.Sprintf("db 0x%02X ; разбор не удался", data[i]))
			n = 1
		} else {
			lines = append(lines, s)
		}
		i += n
	}
	out := strings.Join(lines, "\n")
	expl := ExplainLine(lines[len(lines)-1])
	txt := fmt.Sprintf("%s · режим %s · встроенный мини-движок", pluralInsns(len(lines)), modeName[m])
	if expl != "" {
		txt += "\n" + expl
	}
	return out, Result{Text: txt}
}

func pluralInsns(n int) string {
	switch {
	case n%10 == 1 && n%100 != 11:
		return fmt.Sprintf("%d инструкция", n)
	case n%10 >= 2 && n%10 <= 4 && (n%100 < 10 || n%100 >= 20):
		return fmt.Sprintf("%d инструкции", n)
	default:
		return fmt.Sprintf("%d инструкций", n)
	}
}

// objdumpDisasm — разбор через GNU objdump, вывод Intel-синтаксиса.
func objdumpDisasm(data []byte, m Mode) (string, Result) {
	dir := workDir()
	binPath := filepath.Join(dir, fmt.Sprintf("dis_%d.bin", os.Getpid()))
	defer os.Remove(binPath)
	if err := os.WriteFile(binPath, data, 0o666); err != nil {
		return "", Result{Text: "Нет доступа к временному каталогу", IsErr: true}
	}
	toolObjd, _ := mustTool("objdump")
	cmd := exec.Command(toolObjd, "-D", "-b", "binary", "-m", modeArch[m],
		"-M", "intel", "--insn-width=16", binPath)
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return "", Result{Text: "objdump не найден или ошибка: " + firstLine(stderr.String()), IsErr: true}
	}
	p := stdout.String()
	i := strings.Index(p, "Disassembly of section")
	if i < 0 {
		return "", Result{Text: "objdump: нет секции кода", IsErr: true}
	}
	var lines []string
	for _, ln := range strings.Split(p[i:], "\n")[1:] {
		if strings.HasPrefix(ln, "Disassembly") {
			continue
		}
		if strings.Contains(ln, "file format") {
			break
		}
		insn, _, ok := parseObjdumpLine(ln)
		if ok {
			lines = append(lines, insn)
		}
	}
	if len(lines) == 0 {
		return "", Result{Text: "Не удалось разобрать байты (objdump вернул пусто)", IsErr: true}
	}
	return strings.Join(lines, "\n"), Result{}
}

// parseObjdumpLine разбирает строку вида
//
//	"  401115:  48 01 c3        add    rbx,rax"
//
// возвращает инструкцию и байты кодирования.
func parseObjdumpLine(ln string) (insn string, opBytes []byte, ok bool) {
	c := strings.IndexByte(ln, ':')
	if c < 0 {
		return "", nil, false
	}
	rest := ln[c+1:]
	// байты: идущие пары hex
	fields := strings.Fields(rest)
	k := 0
	var bs []byte
	for k < len(fields) {
		f := fields[k]
		if isHexString(f) && len(f) == 2 {
			v, _ := strconv.ParseUint(f, 16, 8)
			bs = append(bs, byte(v))
			k++
			continue
		}
		break
	}
	if len(bs) == 0 {
		return "", nil, false
	}
	tail := strings.Join(fields[k:], " ")
	if h := strings.IndexByte(tail, '#'); h >= 0 {
		tail = tail[:h]
	}
	tail = strings.TrimSpace(tail)
	if tail == "" {
		return "", nil, false
	}
	// нормализация пробелов и запятых
	var b strings.Builder
	lastSpace := true
	for _, r := range tail {
		if r == ' ' || r == '\t' {
			if !lastSpace {
				b.WriteByte(' ')
				lastSpace = true
			}
			continue
		}
		lastSpace = false
		if r == ',' {
			b.WriteString(", ")
			continue
		}
		b.WriteRune(r)
	}
	return strings.TrimSpace(b.String()), bs, true
}

func isHexString(s string) bool {
	for len(s) > 0 && (s[0] >= '0' && s[0] <= '9' || s[0] >= 'a' && s[0] <= 'f' || s[0] >= 'A' && s[0] <= 'F') {
		return true
	}
	return false
}

// parseHex принимает "B8 01 00", "0xB8,0x01", "B8h 0100" и т.п.
func parseHex(s string) ([]byte, error) {
	var out []byte
	f := strings.FieldsFunc(s, func(r rune) bool {
		return r == ' ' || r == '\t' || r == '\n' || r == ',' || r == ';'
	})
	for _, tok := range f {
		t := tok
		if len(t) >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X') {
			t = t[2:]
		}
		if len(t) > 0 && (t[0] == '$') {
			t = t[1:]
		}
		if len(t) > 0 && (t[len(t)-1] == 'h' || t[len(t)-1] == 'H') {
			t = t[:len(t)-1]
		}
		if len(t)%2 == 1 {
			t = "0" + t
		}
		for i := 0; i+2 <= len(t); i += 2 {
			v, err := strconv.ParseUint(t[i:i+2], 16, 8)
			if err != nil {
				return nil, fmt.Errorf("не-hex токен %q", tok)
			}
			out = append(out, byte(v))
		}
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("пусто")
	}
	return out, nil
}

// ─────────────────────────── разбор по полям ───────────────────────────

// InsnFields — поля машинной команды для таблицы разбора.
type InsnFields struct {
	Prefix   string
	Opcode   string
	Operands string
	Imm      string
	Size     int // байт; -1 — неизвестно
}

// Breakdown строит таблицу разбора кода (из asm или hex) по полям.
func Breakdown(src string, m Mode, isHex bool) (table string, summary string, res Result) {
	var data []byte
	if isHex {
		d, err := parseHex(src)
		if err != nil {
			return "", "", Result{Text: "Не нашёл ни одного байта в hex-строке", IsErr: true}
		}
		data = d
	} else {
		hx, r := Assemble(src, m)
		if r.IsErr {
			return "", "", r
		}
		d, err := parseHex(hx)
		if err != nil {
			return "", "", Result{Text: err.Error(), IsErr: true}
		}
		data = d
	}

	type row struct {
		insn string
		size int
	}
	var rows []row
	if haveTool("objdump") {
		out, r := objdumpDisasm(data, m)
		if r.IsErr {
			return "", "", r
		}
		// размеры возьмём из objdump повторно — распарсим исходный вывод
		sizes := objdumpSizes(data, m)
		lines := strings.Split(out, "\n")
		for i, ln := range lines {
			sz := -1
			if i < len(sizes) {
				sz = sizes[i]
			}
			rows = append(rows, row{ln, sz})
		}
	} else {
		for i := 0; i < len(data); {
			s, n := miniDisasm(data[i:], m)
			if n <= 0 {
				rows = append(rows, row{fmt.Sprintf("db 0x%02X", data[i]), 1})
				i++
			} else {
				rows = append(rows, row{s, n})
				i += n
			}
		}
	}

	var b strings.Builder
	b.WriteString(fmt.Sprintf("%-4s | %-7s | %-8s | %-22s | %-10s | %-4s\n",
		"N", "Prefix", "Opcode", "Operands", "Immediate", "Size"))
	b.WriteString("----+---------+----------+------------------------+------------+-----\n")
	for i, rw := range rows {
		f := SplitInsn(rw.insn, m)
		f.Size = rw.size
		sz := "-"
		if f.Size >= 0 {
			sz = strconv.Itoa(f.Size)
		}
		ops := f.Operands
		if ops == "" {
			ops = "-"
		}
		imm := f.Imm
		if imm == "" {
			imm = "-"
		}
		b.WriteString(fmt.Sprintf("%-4d | %-7s | %-8s | %-22s | %-10s | %-4s\n",
			i+1, f.Prefix, f.Opcode, trunc(ops, 22), trunc(imm, 10), sz))
	}
	total := 0
	for _, rw := range rows {
		if rw.size > 0 {
			total += rw.size
		}
	}
	summary = fmt.Sprintf("%s · режим %s · всего %d байт", pluralInsns(len(rows)), modeName[m], total)
	return b.String(), summary, Result{}
}

// objdumpSizes — размеры инструкций из «сырого» вывода objdump.
func objdumpSizes(data []byte, m Mode) []int {
	dir := workDir()
	binPath := filepath.Join(dir, fmt.Sprintf("bd_%d.bin", os.Getpid()))
	os.WriteFile(binPath, data, 0o666)
	defer os.Remove(binPath)
	toolObjd, _ := mustTool("objdump")
	cmd := exec.Command(toolObjd, "-D", "-b", "binary", "-m", modeArch[m],
		"-M", "intel", "--insn-width=16", binPath)
	var stdout bytes.Buffer
	cmd.Stdout = &stdout
	if cmd.Run() != nil {
		return nil
	}
	p := stdout.String()
	i := strings.Index(p, "Disassembly of section")
	if i < 0 {
		return nil
	}
	var sizes []int
	for _, ln := range strings.Split(p[i:], "\n")[1:] {
		if strings.HasPrefix(ln, "Disassembly") {
			continue
		}
		if strings.Contains(ln, "file format") {
			break
		}
		_, bs, ok := parseObjdumpLine(ln)
		if ok {
			sizes = append(sizes, len(bs))
		}
	}
	return sizes
}

// SplitInsn разбирает текст команды на поля.
func SplitInsn(line string, m Mode) InsnFields {
	f := InsnFields{Size: -1}
	// отсечь комментарий
	if c := strings.IndexByte(line, ';'); c >= 0 {
		line = line[:c]
	}
	line = strings.TrimSpace(line)
	if line == "" {
		return f
	}
	fields := strings.Fields(line)
	op := strings.ToLower(fields[0])
	rest := strings.TrimSpace(strings.TrimPrefix(line, fields[0]))
	switch op {
	case "rep", "repe", "repz", "repne", "repnz", "lock":
		f.Prefix = op
		if rest != "" {
			rf := strings.Fields(rest)
			op = strings.ToLower(rf[0])
			rest = strings.TrimSpace(strings.TrimPrefix(rest, rf[0]))
		}
	}
	f.Opcode = op
	f.Operands = normalizeOps(rest)
	f.Imm = extractImm(f.Operands)
	return f
}

func normalizeOps(s string) string {
	var b strings.Builder
	lastSpace := false
	for _, r := range s {
		if r == ' ' || r == '\t' {
			if b.Len() > 0 && !lastSpace {
				b.WriteByte(' ')
			}
			lastSpace = true
			continue
		}
		lastSpace = false
		if r == ',' {
			b.WriteString(", ")
			continue
		}
		b.WriteRune(r)
	}
	return strings.TrimSpace(b.String())
}

func extractImm(ops string) string {
	depth := 0
	last := 0
	var parts []string
	for i := 0; i <= len(ops); i++ {
		ch := byte(',')
		if i < len(ops) {
			ch = ops[i]
		}
		switch ch {
		case '(', '[':
			depth++
		case ')', ']':
			depth--
		case ',':
			if depth == 0 {
				parts = append(parts, strings.TrimSpace(ops[last:i]))
				last = i + 1
			}
		}
	}
	if len(parts) == 0 {
		return ""
	}
	t := parts[len(parts)-1]
	if t == "" {
		return ""
	}
	c := t[0]
	if (c >= '0' && c <= '9') || c == '$' || c == '\'' {
		return t
	}
	return ""
}

func trunc(s string, n int) string {
	if len(s) > n {
		return s[:n]
	}
	return s
}

// ─────────────────────────── пояснения ───────────────────────────

var explanations = []struct{ op, ru string }{
	{"mov", "Копирование: приёмник = источник"},
	{"lea", "LEA — загрузить эффективный адрес (вычислить адрес, не читая память)"},
	{"add", "Сложение: приёмник = приёмник + источник"},
	{"sub", "Вычитание: приёмник = приёмник - источник"},
	{"inc", "+1 к операнду"},
	{"dec", "-1 от операнда"},
	{"cmp", "Сравнение (вычитает, результат отбрасывает, ставит флаги)"},
	{"test", "Проверка битов (AND без записи результата, ставит флаги)"},
	{"and", "Побитовое И"},
	{"or", "Побитовое ИЛИ"},
	{"xor", "Побитовое XOR (xor eax,eax = обнуление)"},
	{"not", "Инверсия битов"},
	{"neg", "Двоичное дополнение (смена знака)"},
	{"shl", "Сдвиг влево (умножение на 2)"},
	{"shr", "Сдвиг вправо (деление на 2, логический)"},
	{"sar", "Арифметический сдвиг вправо (сохраняет знак)"},
	{"sal", "Сдвиг влево (= shl)"},
	{"push", "Положить значение на стек"},
	{"pop", "Снять значение со стека"},
	{"call", "Вызов процедуры (кладёт адрес возврата в стек)"},
	{"ret", "Возврат из процедуры (POP IP)"},
	{"jmp", "Безусловный переход"},
	{"je", "Переход, если ZF=1 (равно)"},
	{"jne", "Переход, если ZF=0 (не равно)"},
	{"jz", "Переход, если ноль (ZF=1)"},
	{"jnz", "Переход, если не ноль (ZF=0)"},
	{"ja", "Переход, если выше (беззнаковое >)"},
	{"jb", "Переход, если ниже (беззнаковое <)"},
	{"jae", "Переход, если выше или равно"},
	{"jbe", "Переход, если ниже или равно"},
	{"jg", "Переход, если больше (знаковое >)"},
	{"jl", "Переход, если меньше (знаковое <)"},
	{"jge", "Переход, если больше или равно (знаковое)"},
	{"jle", "Переход, если меньше или равно (знаковое)"},
	{"js", "Переход, если знак отрицательный (SF=1)"},
	{"jns", "Переход, если знак положительный (SF=0)"},
	{"jo", "Переход, если переполнение (OF=1)"},
	{"jno", "Переход, если нет переполнения"},
	{"jc", "Переход, если перенос (CF=1)"},
	{"jnc", "Переход, если нет переноса"},
	{"loop", "Цикл: cx/ecx-- , переход пока не ноль"},
	{"mul", "Умножение беззнаковое (AX = AL*src или DX:AX = AX*src)"},
	{"imul", "Умножение со знаком"},
	{"div", "Деление беззнаковое (частное AL / DX:AX)"},
	{"idiv", "Деление со знаком"},
	{"nop", "Нет операции (пауза/выравнивание)"},
	{"hlt", "Остановка процессора (до прерывания)"},
	{"int", "Программное прерывание (DOS: int 0x21 — функции DOS)"},
	{"syscall", "Системный вызов (64-бит Linux)"},
	{"cli", "Запретить маскируемые прерывания (IF=0)"},
	{"sti", "Разрешить прерывания (IF=1)"},
	{"cld", "DF=0 — строковые команды инкрементируют SI/DI"},
	{"std", "DF=1 — строковые команды декрементируют SI/DI"},
	{"movsb", "Пересылка байта строковая (ES:[DI] <- DS:[SI])"},
	{"lodsb", "AL <- [DS:SI]"},
	{"stosb", "[ES:DI] <- AL"},
	{"scasb", "Сравнить AL с [ES:DI]"},
	{"xchg", "Обмен значений"},
	{"cbw", "Знаковое расширение AL -> AX"},
	{"cwd", "Знаковое расширение AX -> DX:AX"},
	{"enter", "Создать кадр стека"},
	{"leave", "Разрушить кадр стека (mov sp,bp; pop bp)"},
	{"sete", "SETcc: записать 1/0 в 8-бит регистр по флагу"},
	{"cmov", "Условная пересылка"},
}

// ExplainLine — короткое русское пояснение к последней инструкции блока.
func ExplainLine(text string) string {
	line := lastInsnLine(text)
	if line == "" {
		return ""
	}
	fields := strings.Fields(line)
	op := strings.ToLower(fields[0])
	if op == "rep" || op == "repe" || op == "repne" || op == "lock" {
		return ExplainLine(strings.TrimSpace(line[len(op):]))
	}
	for _, e := range explanations {
		if strings.HasPrefix(op, e.op) {
			return "Пояснение: " + e.ru
		}
	}
	return fmt.Sprintf("Пояснение: команда «%s» (см. справочник NASM)", op)
}

func lastInsnLine(text string) string {
	var last string
	for _, ln := range strings.Split(text, "\n") {
		ln = strings.TrimSpace(ln)
		if ln == "" || strings.HasSuffix(ln, ":") {
			continue
		}
		last = ln
	}
	return last
}
