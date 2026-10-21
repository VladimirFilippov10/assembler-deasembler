/* engine.c — движок ASM Workshop: вызовы nasm и objdump + пояснения.
 * Только C, без Python. Работает с NASM (синтаксис Intel) и GNU binutils.
 */
#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#define TMPDIR "/tmp/asmworkshop"

static int nasm_to_bytes(const char *src_path, Mode mode __attribute__((unused)),
                         unsigned char *bytes, size_t *len, size_t bytecap,
                         char *err, size_t errcap);

static const char *mode_bits[]  = { "16", "16", "32", "64" };
static const char *mode_arch[]  = { "i8086", "i8086", "i386", "i386:x86-64" };
static const char *mode_name[]  = { "COM 16-bit (DOSBox)", "16-bit", "32-bit", "64-bit" };

/* ─────────────────────────── утилиты ─────────────────────────── */

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int ensure_tmpdir(void)
{
    struct stat st;
    if (stat(TMPDIR, &st) == 0) return 0;
    return mkdir(TMPDIR, 0777);
}

/* Прогнать команду через /bin/sh, взять stdout целиком. Возвращает размер или -1. */
static long run_capture(const char *cmd, char *buf, size_t cap, int *exitcode)
{
    FILE *f = popen(cmd, "r");
    if (!f) { if (exitcode) *exitcode = -1; return -1; }
    size_t total = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (total + 1 < cap) buf[total++] = (char)c;
    }
    buf[total] = 0;
    int rc = pclose(f);
    if (exitcode) *exitcode = (rc >= 0) ? WEXITSTATUS(rc) : -1;
    return (long)total;
}

/* Убрать комментарии ; и # из строки (для определения "пустых"/меточных строк). */
static int line_is_label_only(const char *l)
{
    char b[256];
    strncpy(b, l, sizeof b - 1); b[sizeof b - 1] = 0;
    /* отсечь комментарий */
    for (char *p = b; *p; p++)
        if (*p == ';' || (p == b && *p == '#')) { *p = 0; break; }
    trim(b);
    size_t n = strlen(b);
    if (n == 0) return 0;
    if (b[n-1] != ':') return 0;
    /* метка: identifier без пробелов */
    for (size_t i = 0; i + 1 < n; i++)
        if (isspace((unsigned char)b[i]) || b[i] == ':' ) return 0;
    return isalpha((unsigned char)b[0]) || b[0] == '_' || b[0] == '.';
}

/* Строка — директива (section/bits/global/%)? */
static int line_is_directive(const char *l)
{
    char b[256];
    strncpy(b, l, sizeof b - 1); b[sizeof b - 1] = 0;
    trim(b);
    const char *dirs[] = {"bits","section","segment","global","extern","org","cpu",
                          "db","dw","dd","dq","resb","resw","resd","times","default", NULL};
    char first[16] = {0};
    sscanf(b, "%15s", first);
    for (int i = 0; first[i]; i++) first[i] = (char)tolower((unsigned char)first[i]);
    for (int i = 0; dirs[i]; i++)
        if (strcmp(first, dirs[i]) == 0) return 1;
    return b[0] == '%' || b[0] == '#';
}

/* Есть ли в тексте метки-переходы (jmp/je/... на слово)? */
static int has_jump(const char *text)
{
    static const char *ops[] = {"jmp","je","jne","jz","jnz","ja","jb","jae","jbe",
                                "jc","jnc","jo","jno","js","jns","jp","jnp","jcxz",
                                "jecxz","loop","loope","loopne","call", NULL};
    const char *p = text;
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        char w[16] = {0};
        sscanf(p, "%15s", w);
        for (int i = 0; w[i]; i++) w[i] = (char)tolower((unsigned char)w[i]);
        for (int i = 0; ops[i]; i++)
            if (strcmp(w, ops[i]) == 0) return 1;
    }
    return 0;
}

/* ─────────────────────────── ассемблирование ─────────────────────────── */

static void bytes_to_hex(const unsigned char *d, size_t n, char *out, size_t cap)
{
    size_t pos = 0;
    for (size_t i = 0; i < n && pos + 4 < cap; i++)
        pos += (size_t)snprintf(out + pos, cap - pos, "%s%02X", i ? " " : "", d[i]);
    out[pos] = 0;
}

/* Запустить nasm на файле src_path, получить байты. */
static int nasm_to_bytes(const char *src_path, Mode mode,
                         unsigned char *bytes, size_t *len, size_t bytecap,
                         char *err, size_t errcap)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "nasm -f bin -O0 -o %s/out.bin %s 2>%s/nasm.err",
             TMPDIR, src_path, TMPDIR);
    int rc = 0;
    char junk[512];
    run_capture(cmd, junk, sizeof junk, &rc);
    if (rc != 0) {
        FILE *f = fopen(TMPDIR "/nasm.err", "rb");
        if (f) { size_t k = fread(err, 1, errcap - 1, f); err[k] = 0; fclose(f); }
        else snprintf(err, errcap, "nasm завершился с кодом %d", rc);
        /* первая полезная строка ошибки */
        char *nl = strchr(err, '\n');
        if (nl) *nl = 0;
        trim(err);
        return -1;
    }
    FILE *f = fopen(TMPDIR "/out.bin", "rb");
    if (!f) { snprintf(err, errcap, "нет файла результата"); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz > bytecap) { snprintf(err, errcap, "неверный размер вывода"); fclose(f); return -1; }
    *len = fread(bytes, 1, (size_t)sz, f);
    fclose(f);
    return 0;
}

/* Собрать текст программы для nasm: bits + org + тело. */
static void build_source(const char *user_text, Mode mode, char *src, size_t cap)
{
    size_t pos = 0;
    pos += (size_t)snprintf(src + pos, cap - pos, "BITS %s\n", mode_bits[mode]);
    if (mode == MODE_COM) pos += (size_t)snprintf(src + pos, cap - pos, "ORG 0x100\n");
    pos += (size_t)snprintf(src + pos, cap - pos, "SECTION .text\n%s\n", user_text);
}

static void g_strlcpy_compat(char *dst, const char *src, size_t cap)
{
    if (cap == 0) return;
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* Подставить реальные адреса меток (итеративная сходимость размеров). */
static int assemble_with_labels(const char *text, Mode mode,
                                unsigned char *bytes, size_t *len, size_t bytecap,
                                char *err, size_t errcap)
{
    /* Копия текста построчно */
    static char lines[128][256];
    int nl = 0;
    {
        static char tmp[32768];
        strncpy(tmp, text, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
        char *s = tmp;
        while (*s && nl < 128) {
            char *e = strchr(s, '\n');
            size_t l = e ? (size_t)(e - s) : strlen(s);
            if (l >= 256) l = 255;
            memcpy(lines[nl], s, l); lines[nl][l] = 0;
            nl++;
            s += l + (e ? 1 : 0);
        }
    }

    long lab_addr[128];
    char lab_name[128][64];
    int nlab = 0;
    for (int i = 0; i < nl; i++) {
        if (line_is_label_only(lines[i])) {
            size_t l = strlen(lines[i]);
            char nm[64];
            if (l - 1 < sizeof nm) {
                memcpy(nm, lines[i], l - 1); nm[l - 1] = 0; trim(nm);
                if (nlab < 128) {
                    strcpy(lab_name[nlab], nm);
                    lab_addr[nlab] = -1;
                    nlab++;
                }
            }
        }
    }

    char path[64];
    snprintf(path, sizeof path, TMPDIR "/in_%d.asm", (int)getpid());

    for (int pass = 0; pass < 6; pass++) {
        /* 1) собрать текст с подстановкой известных адресов меток */
        static char src[40000];
        size_t pos = 0;
        pos += (size_t)snprintf(src + pos, sizeof src - pos, "BITS %s\n", mode_bits[mode]);
        if (mode == MODE_COM) pos += (size_t)snprintf(src + pos, sizeof src - pos, "ORG 0x100\n");
        pos += (size_t)snprintf(src + pos, sizeof src - pos, "SECTION .text\n");
        for (int i = 0; i < nl && pos < sizeof src - 300; i++) {
            if (line_is_label_only(lines[i])) continue;
            char ln[512];
            g_strlcpy_compat(ln, lines[i], sizeof ln);
            for (int k = 0; k < nlab; k++) {
                if (lab_addr[k] >= 0) {
                    char *p = ln;
                    while ((p = strstr(p, lab_name[k])) != NULL) {
                        int before_ok = (p == ln) || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
                        char *after = p + strlen(lab_name[k]);
                        int after_ok = !(*after) || !(isalnum((unsigned char)*after) || *after == '_');
                        if (before_ok && after_ok) {
                            char num[32];
                            snprintf(num, sizeof num, "0x%lX", lab_addr[k]);
                            char tail[512];
                            snprintf(tail, sizeof tail, "%s", after);
                            snprintf(p, sizeof ln - (size_t)(p - ln), "%s%s", num, tail);
                            p += strlen(num);
                        } else p = after;
                    }
                } else {
                    /* неизвестная метка: заменить на dummy-адрес, чтобы NASM собрал файл
                     * (реальный адрес появится на следующей итерации) */
                    char *p = ln;
                    while ((p = strstr(p, lab_name[k])) != NULL) {
                        int before_ok = (p == ln) || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
                        char *after = p + strlen(lab_name[k]);
                        int after_ok = !(*after) || !(isalnum((unsigned char)*after) || *after == '_');
                        if (before_ok && after_ok) {
                            char tail[512];
                            snprintf(tail, sizeof tail, "%s", after);
                            snprintf(p, sizeof ln - (size_t)(p - ln), "0x109%s", tail);
                            p += 5;
                        } else p = after;
                    }
                }
            }
            pos += (size_t)snprintf(src + pos, sizeof src - pos, "%s\n", ln);
        }
        FILE *f = fopen(path, "wb");
        fputs(src, f); fclose(f);
        size_t got = 0;
        if (nasm_to_bytes(path, mode, bytes, &got, bytecap, err, errcap) != 0) {
            unlink(path);
            return -1;
        }
        *len = got;

        /* 2) найти адреса меток: собираем префиксы строк [0..i) и берём размер */
        long addr = (mode == MODE_COM) ? 0x100 : 0;
        int changed = 0;
        for (int i = 0; i < nl; i++) {
            if (line_is_label_only(lines[i])) {
                char nm[64];
                size_t l = strlen(lines[i]);
                memcpy(nm, lines[i], l - 1); nm[l - 1] = 0; trim(nm);
                for (int k = 0; k < nlab; k++)
                    if (strcmp(lab_name[k], nm) == 0 && lab_addr[k] != addr) {
                        lab_addr[k] = addr; changed = 1;
                    }
                continue;
            }
            if (line_is_directive(lines[i])) continue;
            /* построить файл-префикс до строки i включительно (без меток) */
            static char pre[40000];
            size_t pp = 0;
            pp += (size_t)snprintf(pre + pp, sizeof pre - pp, "BITS %s\n", mode_bits[mode]);
            if (mode == MODE_COM) pp += (size_t)snprintf(pre + pp, sizeof pre - pp, "ORG 0x100\n");
            pp += (size_t)snprintf(pre + pp, sizeof pre - pp, "SECTION .text\n");
            for (int j = 0; j <= i && pp < sizeof pre - 300; j++) {
                if (line_is_label_only(lines[j])) continue;
                char ln[512];
                g_strlcpy_compat(ln, lines[j], sizeof ln);
                for (int k = 0; k < nlab; k++) {
                    if (lab_addr[k] < 0) continue; /* метка ещё не известна — пропускаем */
                    char *q = ln;
                    while ((q = strstr(q, lab_name[k])) != NULL) {
                        int ok1 = (q == ln) || !(isalnum((unsigned char)q[-1]) || q[-1] == '_');
                        char *a2 = q + strlen(lab_name[k]);
                        int ok2 = !(*a2) || !(isalnum((unsigned char)*a2) || *a2 == '_');
                        if (ok1 && ok2) {
                            char num[32];
                            snprintf(num, sizeof num, "0x%lX", lab_addr[k]);
                            char tail[512];
                            snprintf(tail, sizeof tail, "%s", a2);
                            snprintf(q, sizeof ln - (size_t)(q - ln), "%s%s", num, tail);
                            q += strlen(num);
                        } else q = a2;
                    }
                }
                pp += (size_t)snprintf(pre + pp, sizeof pre - pp, "%s\n", ln);
            }
            char ppre[64];
            snprintf(ppre, sizeof ppre, TMPDIR "/pre_%d.asm", (int)getpid());
            FILE *g = fopen(ppre, "wb"); fputs(pre, g); fclose(g);
            unsigned char tb[65536]; size_t tl = 0; char te[256];
            if (nasm_to_bytes(ppre, mode, tb, &tl, sizeof tb, te, sizeof te) == 0)
                addr = (mode == MODE_COM ? 0x100L : 0L) + (long)tl;
            unlink(ppre);
        }
        if (!changed && pass > 0) break;
    }
    unlink(path);
    return 0;
}

int engine_assemble(const char *src, Mode mode,
                    char *out_hex, size_t hexcap,
                    char *out_msg, size_t msgcap)
{
    if (ensure_tmpdir() != 0) {
        snprintf(out_msg, msgcap, "Не могу создать каталог %s", TMPDIR);
        return -1;
    }
    char text[32768];
    strncpy(text, src, sizeof text - 1); text[sizeof text - 1] = 0;

    /* одна инструкция? тогда просто собрать её */
    unsigned char bytes[65536];
    size_t len = 0;
    char err[512];

    int multi = strchr(text, '\n') != NULL;
    int labels = has_jump(text);

    if (multi && labels) {
        if (assemble_with_labels(text, mode, bytes, &len, sizeof bytes, err, sizeof err) != 0) {
            snprintf(out_msg, msgcap, "NASM: %s", err);
            return -1;
        }
    } else {
        char full[40000];
        build_source(text, mode, full, sizeof full);
        char path[64];
        snprintf(path, sizeof path, TMPDIR "/in_%d.asm", (int)getpid());
        FILE *f = fopen(path, "wb");
        fputs(full, f); fclose(f);
        if (nasm_to_bytes(path, mode, bytes, &len, sizeof bytes, err, sizeof err) != 0) {
            unlink(path);
            snprintf(out_msg, msgcap, "NASM: %s", err);
            return -1;
        }
        unlink(path);
    }

    if (len == 0) {
        snprintf(out_msg, msgcap, "Пустой код (только метки/директивы?)");
        return -1;
    }
    bytes_to_hex(bytes, len, out_hex, hexcap);

    /* Пояснение: длина + режим */
    char expl[256];
    explain_line(text, expl, sizeof expl);
    snprintf(out_msg, msgcap, "%d байт · режим %s%s%s",
             (int)len, mode_name[mode],
             expl[0] ? "\n" : "", expl);
    return 0;
}

/* ─────────────────────────── деассемблирование ─────────────────────────── */

static int hex_pair(const char *s, unsigned char *v)
{
    for (int i = 0; i < 2; i++) {
        if (!isxdigit((unsigned char)s[i])) return -1;
        int d = isdigit(s[i]) ? s[i] - '0' : tolower(s[i]) - 'a' + 10;
        *v = (unsigned char)((*v << 4) | d);
    }
    return 0;
}

static int parse_hex(const char *hexstr, unsigned char *bytes, size_t cap, size_t *len)
{
    size_t n = 0;
    const char *p = hexstr;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (isxdigit((unsigned char)*p)) {
            char pair[3] = {0,0,0};
            int cnt = 0;
            while (*p && isxdigit((unsigned char)*p) && cnt < 2) pair[cnt++] = *p++;
            if (cnt == 1) { /* одиночная цифра — сдвинуть */
                pair[1] = pair[0]; pair[0] = '0';
            }
            unsigned char v = 0;
            if (hex_pair(pair, &v) == 0 && n < cap) bytes[n++] = v;
            continue;
        }
        if (*p == ',' || *p == ';' || *p == '#' || *p == 'x' || *p == 'X') { p++; continue; }
        p++; /* пропустить неизвестное */
    }
    *len = n;
    return n ? 0 : -1;
}

/* Разобрать строку objdump вида:
 *   "  401115:  48 01 c3        add    rbx,rax"
 * -> "add rbx, rax" + стартовые байты. */
static int parse_objdump_line(const char *ln, char *insn, size_t cap,
                              unsigned char *opb, size_t *oplen, size_t opcap)
{
    const char *colon = strchr(ln, ':');
    if (!colon) return -1;
    const char *p = colon + 1;
    /* байты */
    size_t n = 0;
    while (*p && (isspace((unsigned char)*p))) p++;
    while (*p && isxdigit((unsigned char)*p)) {
        char pair[3] = { p[0], p[1], 0 };
        unsigned char v = 0;
        if (pair[1] && hex_pair(pair, &v) == 0) {
            if (n < opcap) opb[n++] = v;
            p += 2;
            while (*p == ' ') p++;
        } else break;
    }
    *oplen = n;
    /* инструкция: до конца строки, отсечь хвост "# addr <sym>" и колонки */
    char raw[256];
    size_t m = 0;
    while (*p && *p != '#' && m < sizeof raw - 1) {
        if (*p == '\t') { raw[m++] = ' '; }
        else raw[m++] = *p;
        p++;
    }
    raw[m] = 0;
    trim(raw);
    if (!raw[0]) return -1;
    /* intel-синтаксис objdump: "add    rbx,rax" -> нормализуем пробелы/запятые */
    char out[256];
    size_t o = 0;
    int lastsp = 1;
    for (size_t i = 0; raw[i] && o < sizeof out - 3; i++) {
        char c = raw[i];
        if (isspace((unsigned char)c)) { if (!lastsp) out[o++] = ' '; lastsp = 1; continue; }
        lastsp = 0;
        if (c == ',') { out[o++] = ','; out[o++] = ' '; continue; }
        out[o++] = c;
    }
    out[o] = 0;
    trim(out);
    snprintf(insn, cap, "%s", out);
    return 0;
}

int engine_disassemble(const char *hexstr, Mode mode,
                       char *out_asm, size_t asmcap,
                       char *out_msg, size_t msgcap)
{
    if (ensure_tmpdir() != 0) {
        snprintf(out_msg, msgcap, "Нет каталога %s", TMPDIR);
        return -1;
    }
    unsigned char bytes[4096];
    size_t len = 0;
    if (parse_hex(hexstr, bytes, sizeof bytes, &len) != 0) {
        snprintf(out_msg, msgcap, "Не нашёл ни одного байта в hex-строке");
        return -1;
    }
    char path[64];
    snprintf(path, sizeof path, TMPDIR "/dis_%d.bin", (int)getpid());
    FILE *f = fopen(path, "wb");
    fwrite(bytes, 1, len, f);
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "objdump -D -b binary -m %s -M intel --insn-width=16 %s 2>/dev/null",
             mode_arch[mode], path);
    char buf[65536];
    int rc = 0;
    run_capture(cmd, buf, sizeof buf, &rc);
    unlink(path);
    if (rc != 0) {
        snprintf(out_msg, msgcap, "objdump не найден или ошибка (код %d)", rc);
        return -1;
    }

    /* найти начало инструкций: строка "Disassembly of section" */
    char *p = strstr(buf, "Disassembly of section");
    if (!p) { snprintf(out_msg, msgcap, "objdump: нет секции кода"); return -1; }
    p = strchr(p, '\n'); if (!p) { snprintf(out_msg, msgcap, "формат objdump?"); return -1; }
    p++;

    size_t apos = 0;
    out_asm[0] = 0;
    int count = 0;
    unsigned char prev_op[16]; size_t prev_len = 0;
    char prev_insn[256] = "";
    (void)prev_op; (void)prev_len;

    char line[512];
    while (*p && apos < asmcap - 300) {
        char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, p, ll); line[ll] = 0;
        p += ll + (e ? 1 : 0);
        if (!e && ll == 0) break;
        if (strncmp(line, "Disassembly", 11) == 0) continue;
        if (strstr(line, "file format")) break;
        unsigned char opb[16]; size_t opl = 0;
        char insn[256];
        if (parse_objdump_line(line, insn, sizeof insn, opb, &opl, sizeof opb) == 0) {
            /* objdump иногда печатает "(bad)" — оставляем как есть */
            count++;
            apos += (size_t)snprintf(out_asm + apos, asmcap - apos, "%s%s",
                                     count > 1 ? "\n" : "", insn);
            strncpy(prev_insn, insn, sizeof prev_insn - 1);
        }
    }
    if (count == 0) {
        snprintf(out_msg, msgcap, "Не удалось разобрать байты (objdump вернул пусто)");
        return -1;
    }
    char expl[256];
    explain_line(prev_insn, expl, sizeof expl);
    snprintf(out_msg, msgcap, "%d инструкци%s · режим %s%s%s",
             count, (count == 1 ? "я" : (count < 5 ? "и" : "й")),
             mode_name[mode], expl[0] ? "\n" : "", expl);
    return 0;
}

/* ─────────────────────────── пояснения ─────────────────────────── */

struct Exp { const char *op; const char *ru; };
static const struct Exp EXPS[] = {
    {"mov",   "Копирование: приёмник = источник"},
    {"lea",   "LEA — загрузить эффективный адрес (вычислить адрес, не читая память)"},
    {"add",   "Сложение: приёмник = приёмник + источник"},
    {"sub",   "Вычитание: приёмник = приёмник - источник"},
    {"inc",   "+1 к операнду"},
    {"dec",   "-1 от операнда"},
    {"cmp",   "Сравнение (вычитает, результат отбрасывает, ставит флаги)"},
    {"test",  "Проверка битов (AND без записи результата, ставит флаги)"},
    {"and",   "Побитовое И"},
    {"or",    "Побитовое ИЛИ"},
    {"xor",   "Побитовое XOR (xor eax,eax = обнуление)"},
    {"not",   "Инверсия битов"},
    {"neg",   "Двоичное дополнение (смена знака)"},
    {"shl",   "Сдвиг влево (умножение на 2)"},
    {"shr",   "Сдвиг вправо (деление на 2, логический)"},
    {"sar",   "Арифметический сдвиг вправо (сохраняет знак)"},
    {"sal",   "Сдвиг влево (= shl)"},
    {"push",  "Положить значение на стек"},
    {"pop",   "Снять значение со стека"},
    {"call",  "Вызов процедуры (кладёт адрес возврата в стек)"},
    {"ret",   "Возврат из процедуры (POP IP)"},
    {"jmp",   "Безусловный переход"},
    {"je",    "Переход, если ZF=1 (равно)"},
    {"jne",   "Переход, если ZF=0 (не равно)"},
    {"jz",    "Переход, если ноль (ZF=1)"},
    {"jnz",   "Переход, если не ноль (ZF=0)"},
    {"ja",    "Переход, если выше (беззнаковое >)"},
    {"jb",    "Переход, если ниже (беззнаковое <)"},
    {"jae",   "Переход, если выше или равно"},
    {"jbe",   "Переход, если ниже или равно"},
    {"jg",    "Переход, если больше (знаковое >)"},
    {"jl",    "Переход, если меньше (знаковое <)"},
    {"jge",   "Переход, если больше или равно (знаковое)"},
    {"jle",   "Переход, если меньше или равно (знаковое)"},
    {"js",    "Переход, если знак отрицательный (SF=1)"},
    {"jns",   "Переход, если знак положительный (SF=0)"},
    {"jo",    "Переход, если переполнение (OF=1)"},
    {"jno",   "Переход, если нет переполнения"},
    {"jc",    "Переход, если перенос (CF=1)"},
    {"jnc",   "Переход, если нет переноса"},
    {"loop",  "Цикл: cx/cx/ecx-- , переход пока не ноль"},
    {"mul",   "Умножение беззнаковое (AX = AL*-src или DX:AX = AX*src)"},
    {"imul",  "Умножение со знаком"},
    {"div",   "Деление беззнаковое (частное AL/DX:AX)"},
    {"idiv",  "Деление со знаком"},
    {"nop",   "Нет операции (пауза/выравнивание)"},
    {"hlt",   "Остановка процессора (до прерывания)"},
    {"int",   "Программное прерывание (DOS: int 0x21 — функции DOS)"},
    {"syscall","Системный вызов (64-бит Linux)"},
    {"cli",   "Запретить маскируемые прерывания (IF=0)"},
    {"sti",   "Разрешить прерывания (IF=1)"},
    {"cld",   "DF=0 — строковые команды инкрементируют SI/DI"},
    {"std",   "DF=1 — строковые команды декрементируют SI/DI"},
    {"rep",   "Повтор строковой команды CX/ECX раз"},
    {"movsb", "Пересылка байта строковая (ES:[DI] <- DS:[SI])"},
    {"lodsb", "AL <- [DS:SI]"},
    {"stosb", "[ES:DI] <- AL"},
    {"scasb", "Сравнить AL с [ES:DI]"},
    {"xchg",  "Обмен значений"},
    {"cbw",   "Знаковое расширение AL -> AX"},
    {"cwd",   "Знаковое расширение AX -> DX:AX"},
    {"enter", "Создать кадр стека"},
    {"leave", "Разрушить кадр стека (mov sp,bp; pop bp)"},
    {"sete",  "SETcc: записать 1/0 в 8-бит регистр по флагу"},
    {"cmov",  "Условный пересылка"},
    {NULL, NULL}
};

void explain_line(const char *line, char *out, size_t cap)
{
    out[0] = 0;
    if (cap == 0) return;
    char b[256];
    strncpy(b, line, sizeof b - 1); b[sizeof b - 1] = 0;
    /* последняя непустая строка блока */
    char *last = NULL;
    for (char *s = b; ; ) {
        trim(s);
        if (s[0] && !line_is_label_only(s)) last = s;
        char *nl = strchr(s, '\n');
        if (!nl) break;
        *nl = 0;
        s = nl + 1;
    }
    if (!last) return;
    char op[16] = {0};
    sscanf(last, "%15s", op);
    for (int i = 0; op[i]; i++) op[i] = (char)tolower((unsigned char)op[i]);
    /* rep/lock префиксы */
    if (strcmp(op, "rep") == 0 || strcmp(op, "lock") == 0 || strcmp(op, "repe") == 0) {
        char rest[256];
        snprintf(rest, sizeof rest, "%s", last + strlen(op));
        trim(rest);
        explain_line(rest, out, cap);
        return;
    }
    for (int i = 0; EXPS[i].op; i++) {
        size_t L = strlen(EXPS[i].op);
        if (strncmp(op, EXPS[i].op, L) == 0) {
            snprintf(out, cap, "Пояснение: %s", EXPS[i].ru);
            return;
        }
    }
    snprintf(out, cap, "Пояснение: команда «%s» (см. справочник NASM)", op);
}
