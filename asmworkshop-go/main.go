// main.go — ASM Workshop (Go): кроссплатформенный запуск GUI или CLI.
//   asmworkshop            — графическое окно (на Windows)
//   asmworkshop --cli      — интерактивный терминальный режим
//   asmworkshop -a "mov ax, 1" [-m com|16|32|64] — собрать строку
//   asmworkshop -d "B8 01 00" [-m ...]           — разобрать байты
package main

import (
	"bufio"
	"fmt"
	"os"
	"strings"
)

func parseMode(s string) Mode {
	switch s {
	case "16":
		return Mode16
	case "32":
		return Mode32
	case "64":
		return Mode64
	default:
		return ModeCOM
	}
}

func usage(p string) {
	fmt.Println("ASM Workshop (Go) — ассемблер ⇄ деасемблер x86")
	fmt.Printf("  %s                 графический интерфейс (Windows)\n", p)
	fmt.Printf("  %s --cli           интерактивный режим в терминале\n", p)
	fmt.Printf("  %s -a \"mov ax, 1\" [-m com|16|32|64]   собрать строку\n", p)
	fmt.Printf("  %s -d \"B8 01 00\"  [-m ...]           разобрать байты\n", p)
	fmt.Printf("  %s -f file.asm    [-m ...]           собрать файл\n", p)
	fmt.Println("Режимы: com (DOSBox, ORG 0x100), 16, 32, 64. По умолчанию com.")
}

func runAsm(src string, m Mode) int {
	hex, res := Assemble(src, m)
	if res.IsErr {
		fmt.Println("\nОШИБКА:", res.Text)
		return 1
	}
	fmt.Println("\nHEX :", hex)
	fmt.Println("ИНФО:", res.Text)
	table, summary, r2 := Breakdown(src, m, false)
	if !r2.IsErr {
		fmt.Print("\nТАБЛИЦА РАЗБОРА (разбиение команды по полям):\n", table)
		fmt.Println("ИТОГ:", summary)
	}
	return 0
}

func runDis(hx string, m Mode) int {
	a, res := Disassemble(hx, m)
	if res.IsErr {
		fmt.Println("\nОШИБКА:", res.Text)
		return 1
	}
	fmt.Println("\nASM :\n" + a)
	fmt.Println("ИНФО:", res.Text)
	table, summary, r2 := Breakdown(hx, m, true)
	if !r2.IsErr {
		fmt.Print("\nТАБЛИЦА РАЗБОРА (разбиение команды по полям):\n", table)
		fmt.Println("ИТОГ:", summary)
	}
	return 0
}

func cliLoop() int {
	mode := ModeCOM
	dir := 0 // 0 = asm->hex, 1 = hex->asm
	showTable := true
	fmt.Println("ASM Workshop CLI (Go) · 'q' выход, '!hex'/'!asm' направление, '!com/!16/!32/!64' режим, 'help' справка")
	in := bufio.NewScanner(os.Stdin)
	for {
		prompt := "asm→hex> "
		if dir == 1 {
			prompt = "hex→asm> "
		}
		fmt.Print(prompt)
		if !in.Scan() {
			break
		}
		line := strings.TrimSpace(in.Text())
		switch line {
		case "":
			continue
		case "q", "quit":
			return 0
		case "help":
			fmt.Println(`  q / quit      выход
  help          эта справка
  !hex          направление asm -> hex
  !asm          направление hex -> asm
  !com/!16/!32/!64  смена режима
  table on/off  таблица разбора по полям`)
			continue
		case "table on":
			showTable = true
			continue
		case "table off":
			showTable = false
			continue
		case "!hex":
			dir = 0
			continue
		case "!asm":
			dir = 1
			continue
		case "!com":
			mode = ModeCOM
			continue
		case "!16":
			mode = Mode16
			continue
		case "!32":
			mode = Mode32
			continue
		case "!64":
			mode = Mode64
			continue
		}
		if dir == 1 {
			_, r := Disassemble(line, mode)
			if r.IsErr {
				fmt.Println("ОШИБКА:", r.Text)
				continue
			}
			if showTable {
				runDis(line, mode)
			} else {
				a, _ := Disassemble(line, mode)
				fmt.Println("ASM :\n" + a)
			}
		} else {
			if showTable {
				runAsm(line, mode)
			} else {
				hex, r := Assemble(line, mode)
				if r.IsErr {
					fmt.Println("ОШИБКА:", r.Text)
					continue
				}
				fmt.Println("HEX :", hex)
			}
		}
	}
	return 0
}

func main() {
	exe, _ := os.Executable()
	p := "asmworkshop"
	if exe != "" {
		if s := lastPathElem(exe); s != "" {
			p = s
		}
	}
	mode := ModeCOM
	var asmsrc, hexsrc, file string
	cli := false
	for i := 1; i < len(os.Args); i++ {
		a := os.Args[i]
		next := func() string {
			i++
			if i < len(os.Args) {
				return os.Args[i]
			}
			return ""
		}
		switch a {
		case "-m":
			mode = parseMode(next())
		case "-a":
			asmsrc = next()
		case "-d":
			hexsrc = next()
		case "-f":
			file = next()
		case "--cli":
			cli = true
		case "-h", "--help":
			usage(p)
			return
		}
	}
	if asmsrc != "" {
		os.Exit(runAsm(asmsrc, mode))
	}
	if hexsrc != "" {
		os.Exit(runDis(hexsrc, mode))
	}
	if file != "" {
		data, err := os.ReadFile(file)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		os.Exit(runAsm(string(data), mode))
	}
	if cli || !guiAvailable() {
		if !cli {
			fmt.Println("Графический интерфейс недоступен в этой сборке — запускаю CLI.")
			fmt.Println("(На Windows соберите asmworkshop.exe для GUI.)")
		}
		os.Exit(cliLoop())
	}
	guiMain()
}

func lastPathElem(s string) string {
	s = strings.ReplaceAll(s, "\\", "/")
	if i := strings.LastIndexByte(s, '/'); i >= 0 {
		return s[i+1:]
	}
	return s
}
