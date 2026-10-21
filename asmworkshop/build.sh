#!/bin/sh
# Сборка ASM Workshop (чистый C + GTK3). Требуется: gcc, pkg-config, libgtk-3-dev, nasm, binutils.
set -e
cd "$(dirname "$0")"
CFLAGS="-O2 -Wall -Wextra $(pkg-config --cflags gtk+-3.0)"
LIBS="$(pkg-config --libs gtk+-3.0)"
gcc $CFLAGS -o asmworkshop src/gui.c src/engine.c $LIBS
gcc -O2 -Wall -Wextra -o asmworkshop_cli src/cli.c src/engine.c
echo "Готово: ./asmworkshop (GUI) и ./asmworkshop_cli (терминал)"
