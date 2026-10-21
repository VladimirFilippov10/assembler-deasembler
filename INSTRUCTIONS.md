# ASM Workshop — инструкции по запуску

Минималистичный ассемблер/деассемблер (NASM, синтаксис Intel) в двух реализациях:
- **Python** (`asm_workshop.py`) — веб-, CLI- и GUI-режимы;
- **C + GTK3** (`asmworkshop/`) — нативные GUI- и CLI-версии.

---

## 1. Python-версия (`asm_workshop.py`)

### Требования
- Python 3.8+ (только стандартная библиотека, зависимостей через pip нет);
- **NASM** — необходим для ассемблирования;
- **objdump** (GNU binutils) — желателен для деассемблирования (если недоступен, используется встроенная база известных кодов);
- *(опционально)* `customtkinter` — только для режима `--gui`.

### Установка зависимостей

Ubuntu / Debian:
```bash
sudo apt install nasm binutils python3
pip install customtkinter        # опционально, для --gui
```

macOS (Homebrew):
```bash
brew install nasm binutils python3
pip install customtkinter        # опционально
```

Windows:
- Установите NASM: https://www.nasm.us/pub/nasm/releasebuilds/ (добавьте в PATH);
- objdump входит в Git for Windows / MSYS2 (добавьте в PATH);
- Python: https://www.python.org/downloads/

### Запуск

```bash
cd /workspace

# Веб-интерфейс (режим по умолчанию, открывает браузер на localhost)
python3 asm_workshop.py

# Веб-интерфейс на указанном порту, без автооткрытия браузера
python3 asm_workshop.py --port 8080 --no-browser

# Текстовый интерфейс в терминале
python3 asm_workshop.py --cli

# Нативное GUI-окно (требуется customtkinter)
python3 asm_workshop.py --gui
```

Полезные команды проверки:
```bash
nasm -v          # проверить наличие NASM
objdump --version   # проверить наличие objdump
python3 asm_workshop.py --help
```

### Режимы (выбираются в интерфейсе)
| Код | Назначение |
|-----|------------|
| `com` | COM (.COM 16-bit, DOSBox) — байты можно вставлять в debug.exe / .COM-файл |
| `16`  | 16-битный raw binary |
| `32`  | 32-битный raw binary |
| `64`  | 64-битный raw binary |

---

## 2. C + GTK3-версия (`asmworkshop/`)

### Требования
gcc, pkg-config, libgtk-3-dev, а также nasm и binutils (как выше).

Установка сборочных зависимостей (Ubuntu/Debian):
```bash
sudo apt install gcc pkg-config libgtk-3-dev nasm binutils
```

### Сборка и запуск
```bash
cd /workspace/asmworkshop
sh build.sh

./asmworkshop        # GUI-версия
./asmworkshop_cli    # версия для терминала
```

---

## Частые проблемы
- **«NASM not found»** — установите NASM и убедитесь, что он доступен в `PATH`.
- **Деассемблирование неточно** — установите binutils (`objdump`); без него работает урезанная встроенная база кодов.
- **`--gui` не запускается** — выполните `pip install customtkinter`.
- **Порт занят** — укажите другой порт: `--port 8080`.
