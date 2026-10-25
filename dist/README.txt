ASM Workshop — готовые сборки (x86-ассемблер ⇄ деасемблер)

WINDOWS 10/11 x64 (графический интерфейс):
  asmworkshop-win64.zip -> распакуйте, запустите asmworkshop.exe
  GUI нативный (Win32), дополнительных DLL не требуется.
  Для ассемблирования через NASM положите nasm.exe в ту же папку или в PATH;
  для дизассемблирования — objdump.exe (из MinGW binutils). Без них работает
  встроенный мини-ассемблер базовых инструкций.

LINUX x64 (CLI + встроенный разбор, без зависимостей):
  linux/asmworkshop-linux-amd64   запуск: chmod +x и ./asmworkshop-linux-amd64 --cli
  linux/asmworkshop-linux-arm64   то же для ARM64
LINUX x64 (GTK3 графический интерфейс, исходники на C):
  linux/asmworkshop-gtk-linux-amd64   требует установленные библиотеки GTK3
  linux/asmworkshop-cli-linux-amd64   терминальная версия той же C-программы

macOS (Intel и Apple Silicon) — CLI-версия Go-программы:
  macos/asmworkshop-darwin-amd64   Intel Mac
  macos/asmworkshop-darwin-arm64   Apple Silicon (M1/M2/M3)
  Если Gatekeeper блокирует запуск: right-click -> Open, либо
  xattr -d com.apple.quarantine ./asmworkshop-darwin-*

Общий флаг справки:  asmworkshop --help / -h
