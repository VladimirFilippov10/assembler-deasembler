package main

import (
"os/exec"
"strings"
"testing"
)

// сверка мини-кодера с nasm
func TestMiniAgainstNasm(t *testing.T) {
if _, err := exec.LookPath("nasm"); err != nil {
t.Skip("no nasm")
}
insns := []string{
"mov ax, 1", "mov cx, 5", "mov bx, 0x1234", "mov al, 0xFF",
"add ax, bx", "sub al, 3", "cmp ax, bx", "xor ax, ax", "and al, 0xFF", "or ax, 1",
"push ax", "pop bx", "push 0x10", "int 0x21", "ret", "nop", "hlt", "cld",
"inc cx", "dec dx", "inc ax", "dec bl",
"mov [bx], ax", "mov [bx+si], ax", "mov [bp+4], ax", "mov word [1234h], ax",
"add [bx+si], cx", "mov [di+10], cl",
"mov eax, 5", "add eax, ebx", "push eax", "pop ecx", "inc ecx",
"mov dx, 0xFFFF", "mov ah, 0x4c", "sub sp, 2", "cmp bx, 0", "jcxz 3",
"xchg ax, bx", "not ax", "neg ax", "mul bx", "div cx",
}
for _, ins := range insns {
mods := []Mode{ModeCOM, Mode32}
for _, m := range mods {
got, err := miniAssemble(ins, m)
if err != nil {
t.Errorf("[%s/%d] miniAssemble: %v", ins, m, err)
continue
}
want, err := nasmAssemble(ins, m)
if err != nil {
t.Errorf("[%s/%d] nasm: %v", ins, m, err)
continue
}
if strings.ToUpper(bytesToHex(got)) != strings.ToUpper(bytesToHex(want)) {
t.Errorf("[%s/%d] mini=%s nasm=%s", ins, m, bytesToHex(got), bytesToHex(want))
continue
}
// раунд-трип через мини-деассемблер
s, n := miniDisasm(got, m)
if n != len(got) {
t.Errorf("[%s/%d] roundtrip size %d != %d (%q)", ins, m, n, len(got), s)
} else {
t.Logf("%s -> %s -> %s", bytesToHex(got), ins, s)
}
}
}
}

func nasmAssemble(ins string, m Mode) ([]byte, error) {
bits := "16"
fmtArg := "bin"
org := ""
switch m {
case Mode32:
bits = "32"
case Mode64:
bits = "64"
case ModeCOM:
org = "org 0x100\n"
}
src := "bits " + bits + "\n" + org + ins + "\n"
cmd := exec.Command("nasm", "-f", fmtArg, "-o", "/dev/stdout", "-")
cmd.Stdin = strings.NewReader(src)
var out strings.Builder
out.Reset()
b, err := cmd.Output()
if err != nil {
return nil, err
}
return b, nil
}
