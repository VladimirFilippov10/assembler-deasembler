#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ASM WORKSHOP — минималистичный ассемблер/деасемблер (NASM, синтаксис Intel)

Два режима:
  [1] Ассемблирование : текст инструкции -> байты (hex) + пояснения
  [2] Деассемблирование: hex-байты      -> инструкция + пояснения

Опции запуска:
  --web    запустить локальный веб-интерфейс (по умолчанию)
  --cli    текстовый интерфейс в терминале
  --gui    нативное GUI-окно (CustomTkinter, если установлен)

Функции ассемблирования используют установленный NASM.
Функции деассемблирования работают через objdump (GNU binutils);
если objdump недоступен, используется встроенная база известных кодов.

Совместимо с рабочим процессом DOSBox: выбери "COM (.COM 16-bit)"
и вставляй полученные байты в debug.exe / .COM-файл.
"""

import argparse
import html
import http.server
import re
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import webbrowser
from pathlib import Path

APP_TITLE = "ASM Workshop"
APP_SUB = "Assembler / Disassembler · NASM · Intel syntax"

# ─────────────────────────── логика (общая для GUI/WEB/CLI) ───────────────────────────

MODES = {
    "com": ("COM (.COM 16-bit, DOSBox)", "bin", "i8086", 0x100),
    "16":  ("16-bit raw binary", "bin", "i8086", 0),
    "32":  ("32-bit raw binary", "bin", "i386", 0),
    "64":  ("64-bit raw binary", "bin", "i386:x86-64", 0),
}

HEX_RE = re.compile(r"\b[0-9A-Fa-f]{2}\b")


def find_tool(name):
    return shutil.which(name)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=20, **kw)


class AsmError(Exception):
    pass


def _nasm_bytes(nasm, src):
    with tempfile.TemporaryDirectory() as d:
        fa = Path(d) / "w.asm"
        fb = Path(d) / "w.bin"
        fa.write_text(src, encoding="utf-8")
        p = run([nasm, "-f", "bin", "-o", str(fb), str(fa)])
        if p.returncode != 0:
            msg = (p.stderr or p.stdout or "").strip()
            msg = re.sub(rf"^{re.escape(str(fa))}:", "", msg, flags=re.M).strip()
            raise AsmError(msg or "Ошибка ассемблирования")
        if not fb.exists():
            raise AsmError("NASM не создал файл")
        return fb.read_bytes()


def assemble(text, mode="com"):
    """Инструкция(и) в NASM-синтаксисе -> список байтов."""
    nasm = find_tool("nasm") or find_tool("nasm.exe")
    if not nasm:
        raise AsmError("NASM не найден в PATH. Установите nasm и повторите.")
    bits = {"com": 16, "16": 16, "32": 32, "64": 64}[mode]
    base = 0x100 if mode == "com" else 0
    src = f"BITS {bits}\nORG {base}\n" + text.strip("\n") + "\n"
    data = _nasm_bytes(nasm, src)
    if not data:
        raise AsmError("Пустой результат (проверьте ввод)")
    return list(data)


def assemble_program(text, mode="com"):
    """Собрать весь текст сразу (с метками и т.п.) -> байты."""
    return assemble(text, mode)


def assemble_lines(lines, mode="com"):
    """Собрать построчно, отслеживая адреса; метки и переходы решаются.

    Возвращает список словарей: n, text, bytes, hex, addr, expl, note.
    Ошибку AsmError выбрасывает только если весь блок не собирается
    даже целиком (тогда сообщение NASM максимально полезное).
    """
    base = 0x100 if mode == "com" else 0
    # разбор строк: метка + инструкция
    clean = []
    for raw in lines:
        line = re.sub(r";.*$", "", raw).strip()
        label = None
        m = re.match(r"^([A-Za-z_.?][\w.?$@-]*):\s*(.*)$", line)
        if m:
            label, line = m.group(1), m.group(2).strip()
        clean.append((raw, line, label))

    def is_dir(line):
        return line.lower().startswith(("bits ", "org ")) or \
               bool(re.match(r"^(section|segment|global|extern|%[a-z])", line, re.I))

    # прогоняем до стабилизации: размеры инструкций могут меняться
    # из-за переполнения short-переходов при росте блока
    labels, sizes = {}, {}
    for _ in range(8):
        addr = base
        new_labels, new_sizes = {}, {}
        for i, (raw, line, label) in enumerate(clean):
            if label and label not in new_labels:
                new_labels[label] = addr
            if line and not is_dir(line):
                try:
                    n = len(assemble(_subst(line, new_labels), mode))
                except AsmError:
                    n = sizes.get(i, 1)
                new_sizes[i] = n
                addr += n
        if new_labels == labels and new_sizes == sizes:
            break
        labels, sizes = new_labels, new_sizes

    rows, addr = [], base
    total_bytes = []  # байты всех предшествующих строк — префикс для NASM-эталона
    for i in range(len(clean)):
        raw, line, label = clean[i]
        if label:
            rows.append({"n": i + 1, "text": f"{label}:", "bytes": [], "hex": "",
                         "addr": addr, "expl": f"Метка '{label}' = 0x{addr:X}",
                         "note": ""})
        if not line:
            continue
        if is_dir(line):
            rows.append({"n": i + 1, "text": raw.strip(), "bytes": [], "hex": "",
                         "addr": addr, "expl": "Директива (байтов не создаёт)",
                         "note": ""})
            continue
        mn = line.split()[0].lower()
        b = None
        note = ""
        tgt = line.split()[-1]
        taddr = labels.get(tgt)
        jump_like = mn.startswith("j") or mn in ("loop", "call")
        if jump_like and taddr is not None and taddr >= addr:
            # эталонный вариант: собрать строки[0..i] целиком через NASM —
            # он сам выберет short/near и посчитает относительное смещение
            prefix = "\n".join(_rebuild(clean, j) for j in range(i + 1))
            try:
                full = assemble_program(prefix, mode)
                cand = list(full[len(total_bytes):])
                if addr + len(cand) == taddr:
                    b = cand
                    note = f"метка '{tgt}' = 0x{taddr:X}, смещение {taddr - (addr + len(b)):+d}"
            except AsmError:
                pass
        if b is None:
            subbed = _subst(line, labels)
            b = assemble(subbed, mode)
            if jump_like and taddr is not None:
                note = f"метка '{tgt}' = 0x{taddr:X}, смещение {taddr - (addr + len(b)):+d}"
        rows.append({"n": i + 1, "text": raw.strip(), "bytes": b,
                     "hex": format_hex(b), "addr": addr,
                     "expl": explain_from_asm(line), "note": note})
        total_bytes += b
        addr += len(b)
    return rows


def _rebuild(clean, j):
    """Восстановить исходную строку из (raw, line, label)."""
    _, line, label = clean[j]
    return (f"{label}: " if label else "") + line


def _reference_size(lines, upto_index, mode):
    """Собрать строки[0..upto_index] целиком и вернуть все байты."""
    block = []
    for _, line, label in lines[:upto_index + 1]:
        s = (f"{label}: " if label else "") + line
        block.append(s)
    try:
        return assemble_program("\n".join(block), mode)
    except AsmError:
        return None


def _subst(line, labels):
    """Заменить в конце строки имя метки на её адрес (для переходов)."""
    parts = line.rsplit(None, 1)
    if len(parts) == 2 and parts[1] in labels:
        return f"{parts[0]} 0x{labels[parts[1]]:X}"
    return line


def disassemble_hex(hexstr, mode="com"):
    """Строка hex-байтов -> список инструкций."""
    clean = re.sub(r"[^0-9A-Fa-f]", "", hexstr)
    if len(clean) % 2:
        raise AsmError("Нечётное число hex-цифр — байты должны идти парами")
    try:
        data = bytes.fromhex(clean)
    except ValueError:
        raise AsmError("Некорректные hex-данные")
    if not data:
        raise AsmError("Введите hex-байты, например: B4 0E CD 10 C3")

    _, arch, machine, vma = MODES[mode]
    objdump = find_tool("objdump")
    if objdump:
        with tempfile.TemporaryDirectory() as d:
            fb = Path(d) / "w.bin"
            fb.write_bytes(data)
            cmd = [objdump, "-D", "-b", "binary", "-m", machine, "-M", "intel"]
            if vma:
                cmd.append(f"--adjust-vma={vma}")
            cmd.append(str(fb))
            p = run(cmd)
            lines = []
            for ln in p.stdout.splitlines():
                m = re.match(r"\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)", ln)
                if m and m.group(3):
                    mn = m.group(3).split()[0].rstrip(":")
                    lines.append({
                        "addr": int(m.group(1), 16),
                        "bytes": [int(x, 16) for x in m.group(2).split()],
                        "text": normalize_intel(m.group(3)),
                        "mnemonic": mn.lower(),
                    })
            if lines:
                return lines
    # запасной вариант — встроенная база
    return builtin_disasm(data, vma)


def normalize_intel(s):
    s = s.replace(",", ", ")
    s = re.sub(r"\s+", " ", s).strip()
    # GAS-артефакты -> NASM-стиль
    s = s.replace("%rip", "")
    s = re.sub(r"\bds:", "", s)
    # mov BYTE PTR ds:0x1234,0x5  ->  mov byte [0x1234], 0x5
    def size_mem(m):
        size, addr = m.group(1), m.group(2)
        rest = m.group(3) or ""
        return f"{size.lower()} [{addr}]{rest}"
    s = re.sub(r"\b(BYTE|WORD|DWORD|QWORD) PTR\s*(?:es|cs|ss|ds|fs|gs)?:?([0-9A-Fa-fx]+)(,[^,]*)?",
               size_mem, s)
    s = re.sub(r"\b(BYTE|WORD|DWORD|QWORD) PTR\b", r"\1", s)
    return s


BUILTIN = {
    "90": ("NOP", "нет операции — занимает время/выравнивание"),
    "C3": ("RET", "возврат из процедуры (POP IP)"),
    "CB": ("RETF", "дальний возврат"),
    "CF": ("IRET", "возврат из прерывания"),
    "CC": ("INT3", "точка останова (отладка)"),
    "F4": ("HLT", "останов процессора до прерывания"),
    "FA": ("CLI", "запретить маскируемые прерывания"),
    "FB": ("STI", "разрешить маскируемые прерывания"),
    "FC": ("CLD", "сброс флага направления DF=0"),
    "FD": ("STD", "установка флага направления DF=1"),
    "0E": ("PUSH CS", "положить CS в стек"),
    "1E": ("PUSH DS", "положить DS в стек"),
}


def builtin_disasm(data, vma):
    out, i = [], vma
    while i < len(data):
        b = data[i]
        h = f"{b:02X}"
        if h in BUILTIN:
            txt, expl = BUILTIN[h]
            out.append({"addr": vma + i, "bytes": [b], "text": txt,
                        "mnemonic": txt.split()[0].lower(), "expl": expl})
            i += 1
        else:
            out.append({"addr": vma + i, "bytes": [b], "text": f"DB 0x{h}",
                        "mnemonic": "db"})
            i += 1
    return out


FLAGS_DOC = {
    "CF": "Carry Flag — перенос/заём при арифметике",
    "PF": "Parity Flag — чётность младшего байта результата",
    "AF": "Auxiliary Flag — перенос из младшей тетрады (BCD)",
    "ZF": "Zero Flag — результат равен нулю",
    "SF": "Sign Flag — знак результата (старший бит)",
    "TF": "Trap Flag — пошаговый режим отладки",
    "IF": "Interrupt Flag — разрешение внешних прерываний",
    "DF": "Direction Flag — направление строковых операций (LODS/MOVS/STOS)",
    "OF": "Overflow Flag — знаковое переполнение",
}

REG8 = ["AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH"]
REG16 = ["AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"]
REG32 = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"]
REG64 = ["RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
         "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"]
SEGS = ["ES", "CS", "SS", "DS", "FS", "GS"]

MNEMONIC_DOC = {
    "mov": "Копирование: приёмник = источник. Из памяти в память напрямую нельзя.",
    "lea": "Load Effective Address — вычисляет адрес источника и кладёт его в регистр (не читает память).",
    "add": "Сложение источника с приёмником, результат в приёмнике. Влияет на CF/ZF/SF/OF/PF/AF.",
    "adc": "Сложение с учётом флага переноса CF (для многоточной арифметики).",
    "sub": "Вычитание источника из приёмника. Влияет на флаги; CF=1 если был заём.",
    "sbb": "Вычитание с учётом заёма CF.",
    "cmp": "Вычитание только для флагов, результат отбрасывается. Основа ветвлений.",
    "inc": "Приёмник + 1. Влияет на все флаги КРОМЕ CF.",
    "dec": "Приёмник − 1. Влияет на все флаги КРОМЕ CF.",
    "neg": "Двоичное отрицание (0 − операнд). Используется для получения отрицательных чисел.",
    "mul": "Беззнаковое умножение AL×источник → AX (или AX×источник → DX:AX).",
    "imul": "Знаковое умножение. 1-операндная форма как MUL, 2/3-операндная — обычное произведение.",
    "div": "Беззнаковое деление AX (или DX:AX) на источник; частное в AL/AX, остаток в AH/DX.",
    "idiv": "Знаковое деление.",
    "and": "Побитовое И. Часто применяют для маскирования битов (AND AL, 0x0F).",
    "or": "Побитовое ИЛИ. OR REG,REG обнуляет флаг переноса дешевле MOV.",
    "xor": "Побитовое исключающее ИЛИ. XOR REG,REG — стандартный способ обнулить регистр.",
    "not": "Побитовое инвертирование. НЕ влияет на флаги.",
    "shl": "Сдвиг влево на 1 или CL битов; освободившиеся биты = 0, последний вышедший бит → CF.",
    "shr": "Логический сдвиг вправо (беззнаковый), старшие биты заполняются нулями.",
    "sar": "Арифметический сдвиг вправо — сохраняет знак (для знаковых чисел).",
    "rol": "Циклический сдвиг влево — вышедший бит возвращается в конец.",
    "ror": "Циклический сдвиг вправо.",
    "push": "Запись значения в стек (SP уменьшается на размер операнда).",
    "pop": "Чтение из стека (SP увеличивается). Парная к PUSH операция.",
    "call": "Переход к подпрограмме: возвращаемый адрес кладётся в стек.",
    "ret": "Возврат из подпрограммы: выборка адреса возврата из стека.",
    "jmp": "Безусловный переход.",
    "je": "Переход, если ZF=1 (равно после CMP).",
    "jne": "Переход, если ZF=0 (не равно).",
    "ja": "Переход если выше (беззнаковое >): CF=0 и ZF=0.",
    "jb": "Переход если ниже (беззнаковое <): CF=1.",
    "jg": "Переход если больше (знаковое >).",
    "jl": "Переход если меньше (знаковое <).",
    "jge": "Переход если больше или равно (знаковое).",
    "jle": "Переход если меньше или равно (знаковое).",
    "jc": "Переход если CF=1 (был перенос/заём).",
    "jnc": "Переход если CF=0.",
    "jo": "Переход если OF=1 (переполнение).",
    "jno": "Переход если OF=0.",
    "js": "Переход если SF=1 (результат отрицательный).",
    "jns": "Переход если SF=0.",
    "loop": "Цикл: CX/ECX -= 1 и переход если CX≠0. Не влияет на флаги.",
    "int": "Программное прерывание. В DOS: INT 0x21 — функции API, INT 0x10 — видео BIOS.",
    "nop": "Нет операции (0x90) — выравнивание, задержки, «заглушки».",
    "hlt": "Останов процессора до ближайшего прерывания (только ring 0).",
    "cli": "Запрет маскируемых прерываний (IF=0).",
    "sti": "Разрешение маскируемых прерываний (IF=1).",
    "cld": "DF=0 — строковые команды идут вверх по памяти.",
    "std": "DF=1 — строковые команды идут вниз по памяти.",
    "rep": "Префикс: повторять строковую команду пока CX≠0 (после каждого шага CX--).",
    "movsb": "Пересылка байта [SI]->[DI] (или [RSI]->[RDI]), индексация по DF.",
    "movsw": "Пересылка слова.",
    "stosb": "Запись AL в [DI] с автоинкрементом DI.",
    "lodsb": "Загрузка [SI] в AL с автоинкрементом SI.",
    "scasb": "Сравнение AL с [DI] без записи результата.",
    "test": "AND только для флагов, результат отбрасывается. Проверка битов.",
    "sete": "Установить байт в 1 если ZF=1 (равно).",
    "setne": "Установить байт в 1 если ZF=0.",
    "cbw": "Конвертировать байт AL в слово AX со знаком.",
    "cwd": "Конвертировать слово AX в двойное слово DX:AX со знаком.",
    "in": "Чтение из порта ввода (например IN AL, 0x60 — клавиатура).",
    "out": "Запись в порт вывода (OUT 0x3F8, AL — последовательный порт).",
    "xlat": "Табличный перевод: AL = [BX + AL].",
    "enter": "Создание кадрового окна стека (аналог PUSH BP; MOV BP,SP + выделение места).",
    "leave": "Разрушение кадра (MOV SP,BP; POP BP).",
    "arpl": "Изменить уровень привилегий селектора.",
    "bound": "Проверка индекса массива на выход за границы.",
    "bsr": "Поиск старшего установленного бита.",
    "bsf": "Поиск младшего установленного бита.",
    "bt": "Проверка бита (перенос в CF).",
    "bts": "Проверка и установка бита.",
    "cmpxchg": "Атомарное сравнение и обмен (основа блокировок).",
    "cpuid": "Запрос информации о процессоре.",
    "cqo": "Расширение знака RAX в RDX (64-бит).",
    "cdq": "Расширение знака EAX в EDX (32-бит).",
    "cdqe": "Расширение знака EAX в RAX.",
    "cwde": "Расширение знака AX в EAX.",
    "iret": "Возврат из обработчика прерывания.",
    "ljmp": "Дальний безусловный переход (сегмент:смещение).",
    "lcall": "Дальний вызов.",
    "lret": "Дальний возврат.",
    "lock": "Префикс атомарности для следующей команды (шинная блокировка).",
    "lahf": "Загрузка флагов AH из флагового регистра.",
    "sahf": "Сохранение AH во флаговый регистр.",
    "pushf": "Положить флаговый регистр в стек.",
    "popf": "Восстановить флаговый регистр из стека.",
    "rdmsr": "Чтение модель-специфичного регистра.",
    "wrmsr": "Запись модель-специфичного регистра.",
    "syscall": "Быстрый системный вызов (64-битный Linux/Windows).",
    "sysret": "Возврат из syscall.",
    "ud2": "Недопустимая команда — генерирует исключение #UD.",
    "wait": "Ожидание завершения сопроцессора.",
    "esc": "Экранирование для FPU-команд.",
    "daa": "Decimal Adjust after Addition (BCD).",
    "das": "Decimal Adjust after Subtraction (BCD).",
    "aaa": "ASCII Adjust after Addition.",
    "aas": "ASCII Adjust after Subtraction.",
    "aam": "ASCII Adjust after Multiply.",
    "aad": "ASCII Adjust before Division.",
}


def explain(instr):
    """Вернуть пояснение к разобранным элементам инструкции."""
    parts = [p.strip() for p in instr.split(" ", 1)]
    mnemonic = parts[0].lower().rstrip(":")
    operands = parts[1] if len(parts) > 1 else ""
    notes = []

    doc = MNEMONIC_DOC.get(mnemonic)
    if doc:
        notes.append(doc)

    op_list = split_operands(operands) if operands else []
    for o in op_list:
        ol = o.lower()
        if ol.startswith("0x"):
            try:
                v = int(ol, 16)
                notes.append(f"Немедленное значение {ol} = {v} (десятичное)")
            except ValueError:
                pass
        elif ol.isdigit():
            notes.append(f"Немедленное значение {o}")
        elif ol in ("al", "ah", "cl", "ch", "dl", "dh", "bl", "bh"):
            notes.append(f"8-битный регистр {o.upper()}")
        elif ol in ("ax", "cx", "dx", "bx", "sp", "bp", "si", "di"):
            role = {"ax": "аккумулятор", "cx": "счётчик циклов",
                    "dx": "данные/порт", "bx": "базовый адрес",
                    "sp": "указатель стека", "bp": "указатель кадра",
                    "si": "источник строковых операций",
                    "di": "приёмник строковых операций"}.get(ol, "регистр")
            notes.append(f"16-битный регистр {o.upper()} ({role})")
        elif ol in ("eax", "ebx", "ecx", "edx", "esi", "edi", "esp", "ebp"):
            notes.append(f"32-битный регистр {o.upper()}")
        elif ol.startswith("[") and ol.endswith("]"):
            notes.append(f"Обращение к памяти по адресу {o} (косвенная адресация)")
        elif "," in o:
            pass

    if mnemonic.startswith("int") and op_list:
        num = op_list[0].lower()
        if num == "0x10":
            notes.append("BIOS-видео: настройка экрана, вывод символа (AH=0x0E — tee-вывод)")
        elif num == "0x21":
            notes.append("DOS API: ввод/вывод, работа с файлами (номер функции в AH)")
        elif num == "0x13":
            notes.append("BIOS-диски: чтение/запись секторов")
        elif num == "0x16":
            notes.append("BIOS-клавиатура: чтение кода нажатия")
        elif num == "0x20":
            notes.append("Завершение программы в DOS (Terminate Program)")
        elif num == "0x29":
            notes.append("BIOS: быстрый вывод символа (AL=символ)")

    return "; ".join(notes) if notes else "Нет подробного описания для этой команды"


def split_operands(s):
    res, depth, cur = [], 0, ""
    for ch in s:
        if ch == "[":
            depth += 1
        if ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            res.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        res.append(cur.strip())
    return res


def format_hex(bytes_list, group=1):
    return " ".join(f"{b:02X}" for b in bytes_list)


def analyze_asm_line(line):
    """Для строки ASM вернуть краткую информацию о типах операндов."""
    low = line.lower()
    tags = []
    if re.search(r"\b(al|ah|bl|bh|cl|ch|dl|dh)\b", low):
        tags.append("8-бит")
    if re.search(r"\b(ax|bx|cx|dx|si|di|sp|bp)\b", low):
        tags.append("16-бит")
    if re.search(r"\be(ax|bx|cx|dx|si|di|sp|bp)\b", low):
        tags.append("32-бит")
    if "[" in line:
        tags.append("память")
    if re.search(r"0x[0-9a-f]+|\b\d+\b", line):
        tags.append("константа")
    return ", ".join(tags) if tags else "—"


# ──────────────────────────────── WEB UI ────────────────────────────────

PAGE = """<!doctype html><html lang="ru"><head><meta charset="utf-8">
<title>__TITLE__</title><meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#0f1117;--panel:#161a23;--panel2:#1c2130;--ink:#e6ebf5;--mut:#8b93a7;
--acc:#7aa2ff;--ok:#5ddba5;--warn:#ffb454;--line:#262c3c;--mono:'JetBrains Mono',ui-monospace,'Cascadia Code',Consolas,monospace}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(1200px 600px at 70% -10%,#1a2033 0%,var(--bg) 55%);color:var(--ink);
font-family:'Inter','Segoe UI',system-ui,sans-serif;min-height:100vh;display:flex;flex-direction:column}
header{display:flex;align-items:center;gap:14px;padding:20px 28px;border-bottom:1px solid var(--line)}
.logo{width:38px;height:38px;border-radius:10px;background:linear-gradient(135deg,#7aa2ff,#5ddba5);
display:grid;place-items:center;font-weight:800;color:#0f1117;font-size:15px}
h1{font-size:18px;font-weight:700;letter-spacing:.3px}
.sub{color:var(--mut);font-size:12px;margin-top:2px}
main{display:grid;grid-template-columns:1fr 1fr;gap:20px;padding:24px 28px;max-width:1240px;width:100%;margin:0 auto;flex:1}
@media(max-width:900px){main{grid-template-columns:1fr}}
.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:18px;display:flex;flex-direction:column;gap:12px}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:1.2px;color:var(--mut);font-weight:600}
textarea{width:100%;min-height:150px;background:var(--panel2);border:1px solid var(--line);border-radius:10px;
color:var(--ink);font-family:var(--mono);font-size:14px;padding:12px;resize:vertical;outline:none;line-height:1.6}
textarea:focus{border-color:var(--acc)}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
select,input[type=text]{background:var(--panel2);border:1px solid var(--line);border-radius:8px;color:var(--ink);
padding:8px 12px;font-size:13px;font-family:inherit;outline:none}
button{cursor:pointer;border:none;border-radius:9px;padding:10px 18px;font-size:13px;font-weight:600;
transition:transform .08s ease,filter .15s ease;font-family:inherit}
button:active{transform:scale(.97)}
.primary{background:linear-gradient(135deg,#7aa2ff,#5d7dff);color:#0f1117}
.primary:hover{filter:brightness(1.1)}
.ghost{background:transparent;border:1px solid var(--line);color:var(--mut)}
.ghost:hover{color:var(--ink);border-color:var(--acc)}
.out{background:var(--panel2);border:1px solid var(--line);border-radius:10px;padding:12px;
font-family:var(--mono);font-size:13.5px;line-height:1.7;white-space:pre-wrap;word-break:break-word;
min-height:120px;overflow:auto;max-height:340px}
.err{color:var(--warn)}
table{width:100%;border-collapse:collapse;font-size:13px}
td,th{padding:7px 10px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{color:var(--mut);font-weight:600;font-size:11px;text-transform:uppercase;letter-spacing:1px}
.addr{color:var(--mut);font-family:var(--mono)}
.bts{color:var(--ok);font-family:var(--mono);white-space:nowrap}
.ins{color:var(--ink);font-family:var(--mono);font-weight:600}
.exp{color:var(--mut);font-size:12.5px}
.badge{display:inline-block;padding:2px 8px;border-radius:20px;background:rgba(122,162,255,.15);
color:var(--acc);font-size:11px;margin-left:6px}
footer{padding:14px 28px;border-top:1px solid var(--line);color:var(--mut);font-size:12px;
display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap}
.kbd{background:var(--panel2);border:1px solid var(--line);border-radius:5px;padding:1px 6px;font-family:var(--mono);font-size:11px}
.copy{font-size:11px;padding:6px 10px}
</style></head><body>
<header>
 <div class="logo">λ</div>
 <div><h1>__TITLE__ <span class="badge">NASM · Intel</span></h1>
 <div class="sub">__SUB__</div></div>
</header>
<main>
 <section class="card">
  <h2>① Ассемблирование — код → байты</h2>
  <textarea id="ain" placeholder="MOV AH, 0x0E&#10;MOV AL, 'A'&#10;INT 0x10&#10;RET"></textarea>
  <div class="row">
   <select id="amode">__AMODES__</select>
   <button class="primary" onclick="doAsm()">Собрать ▸</button>
   <button class="ghost copy" onclick="copyTxt('aout')">Копировать</button>
  </div>
  <div class="out" id="aout">Результат появится здесь…</div>
 </section>
 <section class="card">
  <h2>② Деассемблирование — байты → код</h2>
  <textarea id="din" placeholder="B4 0E B0 41 CD 10 C3"></textarea>
  <div class="row">
   <select id="dmode">__DMODES__</select>
   <button class="primary" onclick="doDis()">Разобрать ▸</button>
   <button class="ghost copy" onclick="copyTxt('dout')">Копировать</button>
  </div>
  <div class="out" id="dout">Результат появится здесь…</div>
 </section>
</main>
<footer>
 <div>NASM + __ENGINE__ · для DOSBox выбирайте режим COM — байты готовы для debug.exe / .COM</div>
 <div><span class="kbd">Ctrl+Enter</span> — выполнить в активном поле</div>
</footer>
<script>
const $=id=>document.getElementById(id);
function esc(s){return s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]))}
async function post(u,d){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});return r.json()}
function copyTxt(id){navigator.clipboard.writeText($(id).innerText)}
async function doAsm(){
 $('aout').textContent='…';
 const r=await post('/api/assemble',{text:$('ain').value,mode:$('amode').value});
 if(r.error){$('aout').innerHTML='<span class="err">✕ '+esc(r.error)+'</span>';return}
 let h='<table><tr><th>Адрес</th><th>Инструкция</th><th>Байты</th></tr>';
 for(const row of r.rows){h+='<tr><td class="addr">'+(row.addr?('0x'+row.addr.toString(16).toUpperCase().padStart(4,'0')):'')+'</td><td class="ins">'+esc(row.text)+
 (row.expl?'<div class="exp">'+esc(row.expl)+'</div>':'')+(row.note?'<div class="exp" style="color:var(--warn)">↳ '+esc(row.note)+'</div>':'')+
 '</td><td class="bts">'+esc(row.hex)+'</td></tr>'}
 h+='</table><div style="margin-top:10px" class="bts">ВСЕГО: '+esc(r.all)+' ('+r.total+' байт)</div>';
 $('aout').innerHTML=h;
}
async function doDis(){
 $('dout').textContent='…';
 const r=await post('/api/disassemble',{hex:$('din').value,mode:$('dmode').value});
 if(r.error){$('dout').innerHTML='<span class="err">✕ '+esc(r.error)+'</span>';return}
 let h='<table><tr><th>Адрес</th><th>Байты</th><th>Инструкция</th><th>Пояснение</th></tr>';
 for(const i of r.items){h+='<tr><td class="addr">'+i.addr+'</td><td class="bts">'+esc(i.bytes)+
 '</td><td class="ins">'+esc(i.text)+'</td><td class="exp">'+esc(i.expl||'')+'</td></tr>'}
 h+='</table>';
 $('dout').innerHTML=h;
}
for(const [ta,fn] of [['ain',doAsm],['din',doDis]]){
 $(ta).addEventListener('keydown',e=>{if((e.ctrlKey||e.metaKey)&&e.key==='Enter'){e.preventDefault();fn()}});
}
</script></body></html>"""


def render_page():
    def opts(sel):
        return "".join(f'<option value="{k}">{v[0]}</option>' for k, v in MODES.items())
    eng = "objdump" if find_tool("objdump") else "встроенная база"
    return (PAGE.replace("__TITLE__", APP_TITLE)
                 .replace("__SUB__", APP_SUB)
                 .replace("__AMODES__", opts("com"))
                 .replace("__DMODES__", opts("com"))
                 .replace("__ENGINE__", eng))


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="text/html; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.end_headers()
        self.wfile.write(body.encode() if isinstance(body, str) else body)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self._send(200, render_page())
        else:
            self._send(404, "not found")

    def do_POST(self):
        import json
        n = int(self.headers.get("Content-Length", 0))
        try:
            req = json.loads(self.rfile.read(n).decode())
        except Exception:
            return self._json({"error": "плохой JSON"})
        if self.path == "/api/assemble":
            try:
                mode = req.get("mode", "com")
                lines = req["text"].splitlines()
                rows = None
                try:
                    rows = assemble_lines(lines, mode)
                except AsmError:
                    rows = None  # построчно не удалось (например метки) — соберём целиком
                if rows is not None:
                    allb = [b for r in rows for b in r["bytes"]]
                else:
                    data = assemble_program(req["text"], mode)
                    allb = list(data)
                    rows = [{"n": 0, "text": "весь блок собран целиком",
                             "hex": format_hex(allb), "addr": 0x100 if mode == "com" else 0,
                             "expl": "", "note": ""}]
                return self._json({"rows": rows, "all": format_hex(allb),
                                   "total": len(allb)})
            except AsmError as e:
                return self._json({"error": str(e)})
            except Exception as e:
                return self._json({"error": str(e)})
        elif self.path == "/api/disassemble":
            try:
                items = disassemble_hex(req["hex"], req.get("mode", "com"))
                out = []
                for it in items:
                    expl = it.get("expl") or explain(it["text"])
                    out.append({"addr": f"0x{it['addr']:04X}",
                                "bytes": format_hex(it["bytes"]),
                                "text": it["text"], "expl": expl})
                return self._json({"items": out})
            except AsmError as e:
                return self._json({"error": str(e)})
            except Exception as e:
                return self._json({"error": str(e)})
        self._send(404, "no")

    def _json(self, obj):
        import json
        self._send(200, json.dumps(obj, ensure_ascii=False), "application/json")


def explain_from_asm(line):
    """Короткое описание по ASM-строке (для режима ассемблирования)."""
    line = re.sub(r";.*$", "", line).strip()
    if not line:
        return ""
    mn = line.split()[0].lower()
    doc = MNEMONIC_DOC.get(mn, "")
    tag = analyze_asm_line(line)
    return (doc + (f" [{tag}]" if tag != "—" else "")).strip()


def serve_web(port=0, open_browser=True):
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    httpd = socketserver.ThreadingTCPServer(("127.0.0.1", port), Handler)
    real = httpd.server_address[1]
    url = f"http://127.0.0.1:{real}/"
    print(f"➜ {APP_TITLE}: {url}  (Ctrl+C — выход)")
    if open_browser:
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


# ──────────────────────────────── CLI UI ────────────────────────────────

def cli_loop():
    print(f"╔{'═'*56}╗")
    print(f"║ {APP_TITLE:<56} ║")
    print(f"║ {APP_SUB:<56} ║")
    print(f"╚{'═'*56}╝")
    print("Режимы: com | 16 | 32 | 64   Команды: asm / dis / mode / help / quit\n")
    mode = "com"
    while True:
        try:
            cmd = input(f"[{mode}]> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            break
        if cmd in ("quit", "exit", "q"):
            break
        if cmd == "mode":
            m = input("новый режим (com/16/32/64): ").strip().lower()
            if m in MODES:
                mode = m
            continue
        if cmd in ("asm", "a"):
            print("Вводите инструкции пустой строкой:")
            buf = []
            while True:
                l = input("  | ")
                if not l:
                    break
                buf.append(l)
            try:
                rows = assemble_lines(buf, mode)
            except AsmError as e:
                print("  ✗", e); continue
            allb = [b for r in rows for b in r["bytes"]]
            for r in rows:
                a = f"0x{r['addr']:04X}"
                extra = ("  ↳ " + r["note"]) if r["note"] else ""
                print(f"  {a}  {r['text']:<26} -> {r['hex']:<16} ; {r['expl']}{extra}")
            print(f"  ВСЕГО: {format_hex(allb)} ({len(allb)} байт)")
            continue
        if cmd in ("dis", "d"):
            hx = input("hex-байты: ")
            try:
                for it in disassemble_hex(hx, mode):
                    print(f"  0x{it['addr']:04X}  {format_hex(it['bytes']):<12} {it['text']:<24} ; {it.get('expl') or explain(it['text'])}")
            except AsmError as e:
                print("  ✗", e)
            continue
        if cmd in ("help", "h", "?", ""):
            print(__doc__.split(chr(10))[1])
            continue
        # иначе считаем, что это сразу ASM-код
        try:
            b = assemble(cmd, mode)
            print(f"  -> {format_hex(b)}  ; {explain_from_asm(cmd)}")
        except AsmError as e:
            print("  ✗", e)


# ──────────────────────────────── NATIVE GUI ────────────────────────────

GUI_CODE = '''
def run_gui():
    try:
        import customtkinter as ctk
    except ImportError:
        print("CustomTkinter не установлен. Попробуйте: pip install customtkinter")
        print("Или используйте --web / --cli")
        sys.exit(1)

    ctk.set_appearance_mode("dark")
    ctk.set_default_color_theme("blue")

    root = ctk.CTk()
    root.title(APP_TITLE)
    root.geometry("1080x640")
    root.grid_columnconfigure((0, 1), weight=1)
    root.grid_rowconfigure(0, weight=1)

    def make_card(col, title, ph):
        f = ctk.CTkFrame(root, corner_radius=14)
        f.grid(row=0, column=col, padx=(18, 8 if col == 0 else 18), pady=18, sticky="nsew")
        f.grid_columnconfigure(0, weight=1)
        f.grid_rowconfigure(1, weight=1)
        ctk.CTkLabel(f, text=title, font=ctk.CTkFont(size=13, weight="bold")).grid(row=0, column=0, sticky="w", padx=14, pady=(12, 4))
        tin = ctk.CTkTextbox(f, font=("Consolas", 14), wrapword=True)
        tin.insert("1.0", ph)
        tin.grid(row=1, column=0, sticky="nsew", padx=14, pady=6)
        row = ctk.CTkFrame(f, fg_color="transparent")
        row.grid(row=2, column=0, sticky="ew", padx=14, pady=(2, 6))
        sel = ctk.CTkOptionMenu(row, values=[v[0] for v in MODES.values()], width=220)
        sel.set(MODES["com"][0])
        sel.pack(side="left")
        btn = ctk.CTkButton(row, text="Выполнить ▸", width=120)
        btn.pack(side="right", padx=(8, 0))
        out = ctk.CTkTextbox(f, font=("Consolas", 13), state="disabled", wrapword=True)
        out.grid(row=3, column=0, sticky="nsew", padx=14, pady=(6, 14))
        f.grid_rowconfigure(3, weight=2)
        return tin, sel, btn, out

    def set_out(box, text):
        box.configure(state="normal")
        box.delete("1.0", "end")
        box.insert("1.0", text)
        box.configure(state="disabled")

    def key_for_mode(sel):
        inv = {v[0]: k for k, v in MODES.items()}
        return inv[sel.get()]

    a_in, a_sel, a_btn, a_out = make_card(0, "① Ассемблирование — код → байты",
                                          "MOV AH, 0x0E\\nMOV AL, 'A'\\nINT 0x10\\nRET")
    d_in, d_sel, d_btn, d_out = make_card(1, "② Деассемблирование — байты → код",
                                          "B4 0E B0 41 CD 10 C3")

    def do_asm():
        mode = key_for_mode(a_sel)
        lines = a_in.get("1.0", "end").splitlines()
        try:
            rows = assemble_lines(lines, mode)
        except AsmError as e:
            set_out(a_out, "✗ " + str(e)); return
        allb = [b for r in rows for b in r["bytes"]]
        res = []
        for r in rows:
            extra = ("\\n     ↳ " + r["note"]) if r["note"] else ""
            res.append(f"0x{r['addr']:04X}  {r['text']}\\n     -> {r['hex']}\\n     ; {r['expl']}{extra}")
        res.append("\\nВСЕГО: " + format_hex(allb) + f" ({len(allb)} байт)")
        set_out(a_out, "\\n".join(res))

    def do_dis():
        mode = key_for_mode(d_sel)
        try:
            items = disassemble_hex(d_in.get("1.0", "end"), mode)
            res = []
            for it in items:
                expl = it.get("expl") or explain(it["text"])
                res.append(f"0x{it['addr']:04X}  {format_hex(it['bytes']):<12} {it['text']}\\n          ; {expl}")
            set_out(d_out, "\\n".join(res))
        except AsmError as e:
            set_out(d_out, "✗ " + str(e))

    a_btn.configure(command=do_asm)
    d_btn.configure(command=do_dis)
    root.mainloop()
'''


# ──────────────────────────────── main ────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=APP_TITLE)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--web", action="store_true", help="локальный веб-интерфейс (по умолчанию)")
    g.add_argument("--cli", action="store_true", help="текстовый интерфейс")
    g.add_argument("--gui", action="store_true", help="нативное окно CustomTkinter")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    if args.cli:
        cli_loop()
    elif args.gui:
        ns = {}
        exec(GUI_CODE, globals(), ns)
        ns["run_gui"]()
    else:
        serve_web(args.port, open_browser=not args.no_browser)


if __name__ == "__main__":
    main()
