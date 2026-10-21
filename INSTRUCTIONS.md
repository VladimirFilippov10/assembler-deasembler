# Инструкции по запуску — ASM Workshop (C + GTK3)

Проект: `/workspace/asmworkshop`

## Требования

- ОС: Linux (рекомендуется Ubuntu/Debian)
- Компилятор: `gcc`
- Библиотека разработки: `libgtk-3-dev` (GTK 3)
- Сборщик: `make`
- Утилиты для работы ассемблера/дезассемблера:
  - `nasm`
  - `objdump` (из состава `binutils`)

## Установка зависимостей (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y build-essential libgtk-3-dev nasm binutils
```

## Сборка

Из каталога проекта:

```bash
cd /workspace/asmworkshop
./build.sh
```

После успешной сборки появятся исполняемые файлы:

- `asmworkshop`     — GUI-версия (окно на GTK3)
- `asmworkshop_cli` — CLI-версия (для запуска из терминала без графической оболочки)

Если скрипт `build.sh` не имеет прав на исполнение, выполните:

```bash
chmod +x build.sh
```

## Запуск

### GUI-версия

```bash
cd /workspace/asmworkshop
./asmworkshop
```

Требуется запущенный X11/Wayland с поддержкой GTK.

### CLI-версия

```bash
cd /workspace/asmworkshop
./asmworkshop_cli --help
```

CLI позволяет собирать и разбирать код в режимах:

| Режим | Описание |
|-------|----------|
| COM   | 16-битные DOS .COM-файлы (запуск в DOSBox) |
| 16    | 16-битный raw binary |
| 32    | 32-битный raw binary |
| 64    | 64-битный raw binary |

Точный список опций можно посмотреть командой:

```bash
./asmworkshop_cli --help
```

## Частые проблемы

1. **Не найден `nasm`**  
   Установите: `sudo apt install nasm`

2. **Не найден `objdump`**  
   Установите: `sudo apt install binutils`

3. **Ошибка при сборке: `gtk/gtk.h: No such file or directory`**  
   Установите GTK-разработку: `sudo apt install libgtk-3-dev`

4. **GUI не открывается**  
   Убедитесь, что есть графическая сессия (X11 или Wayland). Для сервера/WSL используйте X-сервер или запускайте CLI-версию.

## Быстрая проверка окружения

```bash
which gcc make nasm objdump
pkg-config --modversion gtk+-3.0
```

Все команды должны вернуть пути/версии без ошибок.
