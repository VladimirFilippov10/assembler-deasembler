/**
 * @file engine.c
 * @brief Движок ASM Workshop: NASM для сборки, objdump/ndisasm для разбора.
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
#include <sys/wait.h>

#define TMPDIR "/tmp/asmworkshop"

static int  nasm_to_bytes(const char *src_path, Mode mode,
                          unsigned char *bytes, size_t *len, size_t bytecap,
                          char *err, size_t errcap);

static int  disasm_with_sizes(const unsigned char *bytes, size_t len, Mode mode,
                              char (*insns)[256], int *sizes, int maxn, int *count,
                              char *err, size_t errcap);

static void g_strlcpy_compat(char *dst, const char *src, size_t cap);

static long run_capture(const char *cmd, char *buf, size_t cap, int *exitcode);

static int  hex_pair(const char *s, unsigned char *v);

static int  ndisasm_disasm(const unsigned char *bytes, size_t len, Mode mode,
                           char (*insns)[256], int *sizes, int maxn, int *count,
                           char *err, size_t errcap);

static const char *mode_bits[]     = { "16", "16", "32", "64" };
static const char *mode_arch[]     = { "i8086", "i8086", "i386", "i386:x86-64" };
static const char *mode_name[]     = { "COM 16-bit (DOSBox)", "16-bit", "32-bit", "64-bit" };
static const char *mode_bits_num[] = { "16", "16", "32", "64" };

/**
 * Обрезает ведущие и хвостовые пробелы строки на месте.
 *
 * @param s  строка (модифицируется)
 */
static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

/**
 * Создаёт TMPDIR, если его нет.
 *
 * @return  0 при успехе, -1 при ошибке mkdir/stat
 */
static int ensure_tmpdir(void)
{
    struct stat st;
    if (stat(TMPDIR, &st) == 0) return 0;
    return mkdir(TMPDIR, 0777);
}

/**
 * Безопасная копия строки с гарантированным нулём в конце.
 *
 * @param dst  приёмник
 * @param src  источник
 * @param cap  размер приёмника (включая нулевой байт)
 */
static void g_strlcpy_compat(char *dst, const char *src, size_t cap)
{
    if (cap == 0) return;
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/**
 * Запускает shell-команду, читает её stdout целиком.
 *
 * @param cmd        команда для /bin/sh
 * @param buf        буфер для stdout
 * @param cap        размер буфера
 * @param exitcode   сюда кладётся код выхода (может быть NULL)
 * @return           количество прочитанных байт или -1
 */
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

/**
 * Разбирает два hex-символа в байт.
 *
 * @param s  указатель на 2 hex-символа
 * @param v  сюда кладётся результат (накапливается: *v = (*v << 4) | nibble)
 * @return   0 при успехе, -1 если хотя бы один символ не hex
 */
static int hex_pair(const char *s, unsigned char *v)
{
    for (int i = 0; i < 2; i++) {
        if (!isxdigit((unsigned char)s[i])) return -1;
        int d = isdigit((unsigned char)s[i]) ? s[i] - '0'
                                             : tolower((unsigned char)s[i]) - 'a' + 10;
        *v = (unsigned char)((*v << 4) | d);
    }
    return 0;
}

/**
 * Проверяет, что строка — только метка ("name:").
 *
 * @param l  строка
 * @return   1 если это метка, 0 иначе
 */
static int line_is_label_only(const char *l)
{
    char b[256];
    strncpy(b, l, sizeof b - 1); b[sizeof b - 1] = 0;
    for (char *p = b; *p; p++)
        if (*p == ';' || (p == b && *p == '#')) { *p = 0; break; }
    trim(b);
    size_t n = strlen(b);
    if (n == 0) return 0;
    if (b[n-1] != ':') return 0;
    for (size_t i = 0; i + 1 < n; i++)
        if (isspace((unsigned char)b[i]) || b[i] == ':') return 0;
    return isalpha((unsigned char)b[0]) || b[0] == '_' || b[0] == '.';
}

/**
 * Проверяет, что строка начинается с директивы NASM (bits, section, db, ...).
 *
 * @param l  строка
 * @return   1 если директива, 0 иначе
 */
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

/**
 * Если строка вида "метка: остаток" — копирует имя метки в nm.
 *
 * @param l      строка
 * @param nm     буфер для имени (без ':')
 * @param ncap   размер буфера
 * @return       1 если найдена метка-префикс, 0 иначе
 */
static int line_label_prefix(const char *l, char *nm, size_t ncap)
{
    const char *p = l;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' || *p == '.'))
        return 0;
    const char *s = p;
    while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '.')) p++;
    if (*p != ':') return 0;
    size_t L = (size_t)(p - s);
    if (L == 0 || L >= ncap) return 0;
    memcpy(nm, s, L); nm[L] = 0;
    return 1;
}

/**
 * Копирует в out всё, что идёт после "метка:".
 *
 * @param l     строка с меткой
 * @param out   буфер для остатка
 * @param cap   размер буфера
 */
static void line_after_label(const char *l, char *out, size_t cap)
{
    const char *c = strchr(l, ':');
    if (!c) { out[0] = 0; return; }
    c++;
    while (*c && isspace((unsigned char)*c)) c++;
    g_strlcpy_compat(out, c, cap);
}

/**
 * Копирует первое слово строки (мнемонику) в out, в нижнем регистре.
 *
 * @param line  строка
 * @param out   буфер
 * @param cap   размер буфера
 */
static void first_word(const char *line, char *out, size_t cap)
{
    out[0] = 0;
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t n = 0;
    while (*p && !isspace((unsigned char)*p) && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    for (size_t i = 0; out[i]; i++) out[i] = (char)tolower((unsigned char)out[i]);
}

/**
 * Ищет слово word в line как отдельный токен (границами считаются
 * символы, не входящие в [A-Za-z0-9_.]).
 *
 * @param line  строка
 * @param word  искомое слово
 * @return      1 если найдено, 0 иначе
 */
static int has_word(const char *line, const char *word)
{
    size_t L = strlen(word);
    const char *p = line;
    while ((p = strstr(p, word)) != NULL) {
        int before_ok = (p == line) || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.');
        const char *after = p + L;
        int after_ok = !(*after) || !(isalnum((unsigned char)*after) || *after == '_' || *after == '.');
        if (before_ok && after_ok) return 1;
        p = after;
    }
    return 0;
}

/**
 * Проверяет, есть ли в тексте переход/вызов на объявленную метку.
 * Нужно, чтобы включить итеративную подстановку адресов в assemble_with_labels.
 *
 * @param text  исходник
 * @return      1 если есть ссылки на метки, 0 иначе
 */
static int has_label_refs(const char *text)
{
    char names[128][64];
    int nn = 0;
    {
        char tmp[32768];
        strncpy(tmp, text, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
        char *s = tmp;
        while (*s) {
            char *e = strchr(s, '\n');
            size_t l = e ? (size_t)(e - s) : strlen(s);
            char ln[256];
            if (l >= sizeof ln) l = sizeof ln - 1;
            memcpy(ln, s, l); ln[l] = 0;
            char nm[64];
            int is_lab = line_is_label_only(ln) || line_label_prefix(ln, nm, sizeof nm);
            if (is_lab) {
                if (line_is_label_only(ln)) {
                    size_t m = strlen(ln);
                    if (m >= 1 && m - 1 < sizeof nm) {
                        memcpy(nm, ln, m - 1); nm[m - 1] = 0; trim(nm);
                    } else nm[0] = 0;
                }
                if (nm[0] && nn < 128) strcpy(names[nn++], nm);
            }
            if (!e) break;
            s = e + 1;
        }
    }
    if (nn == 0) return 0;

    static const char *br[] = {"jmp","je","jne","jz","jnz","ja","jb","jae","jbe",
                               "jc","jnc","jo","jno","js","jns","jp","jnp","jcxz",
                               "jecxz","loop","loope","loopne","call", NULL};
    char tmp2[32768];
    strncpy(tmp2, text, sizeof tmp2 - 1); tmp2[sizeof tmp2 - 1] = 0;
    char *s = tmp2;
    while (*s) {
        char *e = strchr(s, '\n');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        char ln[256];
        if (l >= sizeof ln) l = sizeof ln - 1;
        memcpy(ln, s, l); ln[l] = 0;
        char body[256];
        char pfx[64];
        if (line_is_label_only(ln)) { if (!e) break; s = e + 1; continue; }
        if (line_label_prefix(ln, pfx, sizeof pfx)) line_after_label(ln, body, sizeof body);
        else g_strlcpy_compat(body, ln, sizeof body);
        char op[16];
        first_word(body, op, sizeof op);
        int is_branch = 0;
        for (int i = 0; br[i]; i++)
            if (strcmp(op, br[i]) == 0) { is_branch = 1; break; }
        if (is_branch)
            for (int k = 0; k < nn; k++)
                if (has_word(body, names[k])) return 1;
        if (!e) break;
        s = e + 1;
    }
    return 0;
}

/**
 * Переводит байты в hex-строку через пробел.
 *
 * @param d    байты
 * @param n    длина
 * @param out  буфер (cap должен быть >= 3*n+1)
 * @param cap  размер буфера
 */
static void bytes_to_hex(const unsigned char *d, size_t n, char *out, size_t cap)
{
    size_t pos = 0;
    for (size_t i = 0; i < n && pos + 4 < cap; i++)
        pos += (size_t)snprintf(out + pos, cap - pos, "%s%02X", i ? " " : "", d[i]);
    out[pos] = 0;
}

/**
 * Запускает NASM на готовом .asm файле и читает результат.
 *
 * @param src_path  путь к .asm
 * @param mode      режим (уже учтён в BITS/ORG исходника)
 * @param bytes     сюда кладутся байты
 * @param len       сюда кладётся длина
 * @param bytecap   размер буфера bytes
 * @param err       буфер для текста ошибки
 * @param errcap    размер err
 * @return          0 при успехе, -1 при ошибке
 */
static int nasm_to_bytes(const char *src_path, Mode mode,
                         unsigned char *bytes, size_t *len, size_t bytecap,
                         char *err, size_t errcap)
{
    char cmd[1024];
    (void)mode;
    snprintf(cmd, sizeof cmd, "nasm -f bin -O0 -o %s/out.bin %s 2>%s/nasm.err",
             TMPDIR, src_path, TMPDIR);
    int rc = 0;
    char junk[512];
    run_capture(cmd, junk, sizeof junk, &rc);
    if (rc != 0) {
        FILE *f = fopen(TMPDIR "/nasm.err", "rb");
        if (f) { size_t k = fread(err, 1, errcap - 1, f); err[k] = 0; fclose(f); }
        else snprintf(err, errcap, "nasm завершился с кодом %d", rc);
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
    if (sz < 0 || (size_t)sz > bytecap) {
        snprintf(err, errcap, "неверный размер вывода");
        fclose(f);
        return -1;
    }
    *len = fread(bytes, 1, (size_t)sz, f);
    fclose(f);
    return 0;
}

/**
 * Собирает полный исходник для NASM: BITS + (ORG) + SECTION .text + тело.
 *
 * @param user_text  код пользователя
 * @param mode       режим
 * @param src        буфер результата
 * @param cap        размер src
 */
static void build_source(const char *user_text, Mode mode, char *src, size_t cap)
{
    size_t pos = 0;
    pos += (size_t)snprintf(src + pos, cap - pos, "BITS %s\n", mode_bits[mode]);
    if (mode == MODE_COM) pos += (size_t)snprintf(src + pos, cap - pos, "ORG 0x100\n");
    pos += (size_t)snprintf(src + pos, cap - pos, "SECTION .text\n%s\n", user_text);
}

/**
 * Итеративно подставляет реальные адреса меток в код, пока размеры
 * инструкций не перестанут меняться (обычно 2-3 прохода).
 *
 * @param text      исходник с метками
 * @param mode      режим
 * @param bytes     результат
 * @param len       длина результата
 * @param bytecap   размер буфера bytes
 * @param err       буфер ошибки
 * @param errcap    размер err
 * @return          0 при успехе, -1 при ошибке NASM
 */
static int assemble_with_labels(const char *text, Mode mode,
                                unsigned char *bytes, size_t *len, size_t bytecap,
                                char *err, size_t errcap)
{
    static char lines[256][256];
    int nl = 0;
    {
        static char tmp[32768];
        strncpy(tmp, text, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
        char *s = tmp;
        while (*s && nl < 256) {
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
                    /* dummy-адрес 0x109, чтобы NASM не падал на первом проходе
                     * с "symbol not defined" — реальный адрес появится позже */
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
            /* собираем префикс [0..i] и берём его длину как адрес следующей строки */
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
                    if (lab_addr[k] < 0) continue;
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

/**
 * Собирает код пользователя в байты через NASM.
 *
 * @param src      исходник (одна строка или блок)
 * @param mode     режим (COM/16/32/64)
 * @param out_hex  hex-строка результата
 * @param hexcap   размер out_hex
 * @param out_msg  сообщение об успехе или ошибке
 * @param msgcap   размер out_msg
 * @return         0 при успехе, -1 при ошибке
 */
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

    unsigned char bytes[65536];
    size_t len = 0;
    char err[512];

    int multi = strchr(text, '\n') != NULL;
    int labels = has_label_refs(text);

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

    char expl[256];
    explain_line(text, expl, sizeof expl);
    snprintf(out_msg, msgcap, "%d байт · режим %s%s%s",
             (int)len, mode_name[mode],
             expl[0] ? "\n" : "", expl);
    return 0;
}

/**
 * Разбирает hex-строку ("B8 01 00", "0xB8,0x01", "B8h 0100") в байты.
 *
 * @param hexstr  строка
 * @param bytes   буфер для результата
 * @param cap     размер буфера
 * @param len     сюда кладётся длина
 * @return        0 при успехе, -1 если ни одного байта не найдено
 */
static int parse_hex(const char *hexstr, unsigned char *bytes, size_t cap, size_t *len)
{
    size_t n = 0;
    const char *p = hexstr;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; continue; }
        if (*p == '$') { p++; continue; }
        if (isxdigit((unsigned char)*p)) {
            char pair[3] = {0,0,0};
            int cnt = 0;
            while (*p && isxdigit((unsigned char)*p) && cnt < 2) pair[cnt++] = *p++;
            if ((*p == 'h' || *p == 'H') && cnt == 2) p++;
            if (cnt == 1) { pair[1] = pair[0]; pair[0] = '0'; }
            unsigned char v = 0;
            if (hex_pair(pair, &v) == 0 && n < cap) bytes[n++] = v;
            continue;
        }
        if (*p == ',' || *p == ';') { p++; continue; }
        if (*p == 'h' || *p == 'H') { p++; continue; }
        p++;
    }
    *len = n;
    return n ? 0 : -1;
}

/**
 * Разбирает одну строку objdump вида "  401115:  48 01 c3  add rbx,rax".
 *
 * @param ln      строка objdump
 * @param insn    сюда кладётся текст инструкции ("add rbx, rax")
 * @param cap     размер insn
 * @param opb     сюда кладутся байты кодирования
 * @param oplen   сюда кладётся число байт
 * @param opcap   размер opb
 * @return        0 при успехе, -1 если не удалось разобрать
 */
int parse_objdump_line(const char *ln, char *insn, size_t cap,
                       unsigned char *opb, size_t *oplen, size_t opcap)
{
    const char *colon = strchr(ln, ':');
    if (!colon) return -1;
    const char *p = colon + 1;
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

/**
 * Разбирает raw-байты через ndisasm (входит в поставку NASM).
 * Основной fallback, когда objdump не умеет 16-бит или отсутствует.
 *
 * @param bytes   байты
 * @param len     длина
 * @param mode    режим (16/32/64)
 * @param insns   массив строк результата
 * @param sizes   массив длин инструкций
 * @param maxn    максимум строк
 * @param count   сюда кладётся число строк
 * @param err     буфер ошибки
 * @param errcap  размер err
 * @return        0 при успехе, -1 при ошибке
 */
static int ndisasm_disasm(const unsigned char *bytes, size_t len, Mode mode,
                          char (*insns)[256], int *sizes, int maxn, int *count,
                          char *err, size_t errcap)
{
    char path[64];
    snprintf(path, sizeof path, TMPDIR "/nd_%d.bin", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, errcap, "нет доступа к %s", TMPDIR); return -1; }
    fwrite(bytes, 1, len, f); fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof cmd, "ndisasm -b %s -o 0 -u %s 2>/dev/null",
             mode_bits[mode], path);
    static char buf[65536];
    int rc = 0;
    run_capture(cmd, buf, sizeof buf, &rc);
    unlink(path);
    if (rc != 0) { snprintf(err, errcap, "ndisasm не найден или ошибка"); return -1; }

    int n = 0;
    char *p = buf, line[512];
    while (*p && n < maxn) {
        char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, p, ll); line[ll] = 0;
        p += ll + (e ? 1 : 0);

        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        int off = 0;
        while (isxdigit((unsigned char)*s) && off < 8) { s++; off++; }
        if (off != 8) continue;
        while (*s == ' ' || *s == '\t') s++;
        unsigned char opb[32]; int opn = 0;
        while (isxdigit((unsigned char)*s) && opn < 32) {
            if (!isxdigit((unsigned char)s[1])) break;
            char pair[3] = { s[0], s[1], 0 };
            unsigned char v = 0;
            if (hex_pair(pair, &v) != 0) break;
            opb[opn++] = v; s += 2;
        }
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) continue;

        char insn[256]; size_t oi = 0; int lastsp = 1;
        for (char *q = s; *q && oi < sizeof insn - 3; q++) {
            char c = *q;
            if (c == '\t' || c == ' ') {
                if (!lastsp && oi) insn[oi++] = ' ';
                lastsp = 1;
            } else if (c == ',') {
                insn[oi++] = ','; insn[oi++] = ' '; lastsp = 1;
            } else {
                insn[oi++] = c; lastsp = 0;
            }
        }
        insn[oi] = 0;
        while (oi && insn[oi-1] == ' ') insn[--oi] = 0;
        if (insn[0]) {
            g_strlcpy_compat(insns[n], insn, 256);
            sizes[n] = opn;
            n++;
        }
    }
    *count = n;
    if (n == 0) { snprintf(err, errcap, "ndisasm вернул пусто"); return -1; }
    return 0;
}

/**
 * Разбирает байты через objdump, при отказе — через ndisasm.
 *
 * @param bytes   байты
 * @param len     длина
 * @param mode    режим
 * @param insns   массив строк результата
 * @param sizes   массив длин инструкций
 * @param maxn    максимум строк
 * @param count   сюда кладётся число строк
 * @param err     буфер ошибки
 * @param errcap  размер err
 * @return        0 при успехе, -1 при ошибке
 */
static int disasm_with_sizes(const unsigned char *bytes, size_t len, Mode mode,
                             char (*insns)[256], int *sizes, int maxn, int *count,
                             char *err, size_t errcap)
{
    char path[64];
    snprintf(path, sizeof path, TMPDIR "/bd_%d.bin", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, errcap, "нет доступа к %s", TMPDIR); return -1; }
    fwrite(bytes, 1, len, f);
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "objdump -D -b binary -m %s -M intel --insn-width=16 %s 2>/dev/null",
             mode_arch[mode], path);
    static char buf[65536];
    int rc = 0;
    run_capture(cmd, buf, sizeof buf, &rc);
    unlink(path);
    if (rc != 0) {
        if (ndisasm_disasm(bytes, len, mode, insns, sizes, maxn, count, err, errcap) == 0)
            return 0;
        snprintf(err, errcap, "objdump не найден или ошибка (код %d)", rc);
        return -1;
    }
    char *p = strstr(buf, "Disassembly of section");
    if (!p) {
        if (ndisasm_disasm(bytes, len, mode, insns, sizes, maxn, count, err, errcap) == 0)
            return 0;
        snprintf(err, errcap, "objdump: нет секции кода");
        return -1;
    }
    p = strchr(p, '\n');
    if (!p) {
        if (ndisasm_disasm(bytes, len, mode, insns, sizes, maxn, count, err, errcap) == 0)
            return 0;
        snprintf(err, errcap, "формат objdump?");
        return -1;
    }
    p++;

    int n = 0;
    char line[512];
    while (*p && n < maxn) {
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
            g_strlcpy_compat(insns[n], insn, 256);
            sizes[n] = (int)opl;
            n++;
        }
    }
    *count = n;
    if (n == 0) {
        if (ndisasm_disasm(bytes, len, mode, insns, sizes, maxn, count, err, errcap) == 0)
            return 0;
        snprintf(err, errcap, "Не удалось разобрать байты (objdump вернул пусто)");
        return -1;
    }
    return 0;
}

/**
 * Оценивает качество разбора: считает долю "(bad)"/".byte" в выводе.
 *
 * @param bytes   байты
 * @param len     длина
 * @param mode    режим
 * @param bad     сюда кладётся число «плохих» строк
 * @return        число разобранных инструкций или -1
 */
static int disasm_quality(const unsigned char *bytes, size_t len, Mode mode, int *bad)
{
    static char insns[64][256];
    static int sizes[64];
    int count = 0;
    char err[256];
    *bad = 0;
    if (disasm_with_sizes(bytes, len, mode, insns, sizes, 64, &count, err, sizeof err) != 0)
        return -1;
    for (int i = 0; i < count; i++)
        if (strstr(insns[i], ".byte") || strstr(insns[i], "(bad)") ||
            strstr(insns[i], ".short")) (*bad)++;
    return count;
}

/**
 * Если запрошен 16-бит режим, а он даёт мусор — пробует 32/64/16/COM
 * и возвращает режим с минимумом «плохих» строк.
 *
 * @param bytes  байты
 * @param len    длина
 * @param asked  запрошенный режим
 * @return       лучший режим из доступных
 */
static Mode auto_dis_mode(const unsigned char *bytes, size_t len, Mode asked)
{
    if (asked != MODE_COM && asked != MODE_16) return asked;
    int bad_asked = 999;
    int n = disasm_quality(bytes, len, asked, &bad_asked);
    if (n > 0 && bad_asked == 0) return asked;
    const Mode order[] = { MODE_32, MODE_64, MODE_16, MODE_COM };
    Mode best = asked;
    int best_bad = bad_asked, best_n = n;
    for (unsigned k = 0; k < sizeof order / sizeof order[0]; k++) {
        int bad = 0;
        int cnt = disasm_quality(bytes, len, order[k], &bad);
        if (cnt <= 0) continue;
        if (bad < best_bad || (bad == best_bad && cnt > best_n)) {
            best_bad = bad; best_n = cnt; best = order[k];
        }
        if (bad == 0) break;
    }
    return best;
}

/**
 * Деассемблирует hex-строку в текст ASM (по одной инструкции на строку).
 *
 * @param hexstr   hex-байты
 * @param mode     режим
 * @param out_asm  буфер результата
 * @param asmcap   размер out_asm
 * @param out_msg  сообщение об успехе или ошибке
 * @param msgcap   размер out_msg
 * @return         0 при успехе, -1 при ошибке
 */
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
    Mode used = auto_dis_mode(bytes, len, mode);

    static char insns[256][256];
    static int sizes[256];
    int count = 0;
    char err[512];
    if (disasm_with_sizes(bytes, len, used, insns, sizes, 256, &count, err, sizeof err) != 0) {
        snprintf(out_msg, msgcap, "%s", err);
        return -1;
    }

    size_t apos = 0;
    out_asm[0] = 0;
    for (int i = 0; i < count && apos < asmcap - 300; i++) {
        apos += (size_t)snprintf(out_asm + apos, asmcap - apos,
                                 "%s%s", i > 0 ? "\n" : "", insns[i]);
    }

    char expl[256];
    explain_line(insns[count - 1], expl, sizeof expl);
    snprintf(out_msg, msgcap, "%d инструкци%s · режим %s%s%s%s",
             count, (count == 1 ? "я" : (count < 5 ? "и" : "й")),
             mode_name[used], expl[0] ? "\n" : "", expl,
             used != mode ?
             "\nПримечание: запрошенный режим не дал чистого разбора;\n"
             "выбран лучший из доступных (objdump / ndisasm)." : "");
    return 0;
}

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

/**
 * Извлекает immediate-операнд (последний операнд, начинающийся
 * с цифры, 0x, $ или кавычки).
 *
 * @param ops  строка операндов
 * @param out  буфер результата
 * @param cap  размер out
 */
static void extract_imm(const char *ops, char *out, size_t cap)
{
    out[0] = 0;
    const char *last = ops;
    int depth = 0;
    const char *p = ops;
    for (;; p++) {
        if (*p == '(' || *p == '[') depth++;
        else if (*p == ')' || *p == ']') depth--;
        else if ((*p == ',' && depth == 0) || *p == 0) {
            const char *s = last;
            while (s < p && isspace((unsigned char)*s)) s++;
            if (s < p) {
                size_t l = (size_t)(p - s);
                char c0 = ' ';
                { const char *t = s; while (t < p && isspace((unsigned char)*t)) t++; if (t < p) c0 = *t; }
                int isimm = isdigit((unsigned char)c0) || c0 == '$' || c0 == '\'';
                if (!isimm && l >= 2 && c0 == '0' && (s[1] == 'x' || s[1] == 'X')) isimm = 1;
                if (isimm && l < cap) { memcpy(out, s, l); out[l] = 0; trim(out); }
            }
            if (!*p) break;
            last = p + 1;
        }
    }
}

/**
 * Разбивает строку инструкции на поля для таблицы разбора.
 *
 * @param line  инструкция ("rep stosb", "mov ax, 1", ...)
 * @param mode  режим (для поля bits)
 * @param f     сюда кладутся поля
 */
void insn_split(const char *line, Mode mode, InsnFields *f)
{
    memset(f, 0, sizeof *f);
    f->prefix = -1;
    f->size = -1;
    f->bits = atoi(mode_bits_num[mode]);

    char b[512];
    strncpy(b, line, sizeof b - 1); b[sizeof b - 1] = 0;
    for (char *p = b; *p; p++)
        if (*p == ';' || (p == b && *p == '#')) { *p = 0; break; }
    trim(b);
    if (!b[0]) return;

    char *sp = b;
    while (*sp && !isspace((unsigned char)*sp)) sp++;
    size_t opl = (size_t)(sp - b);
    if (opl >= sizeof f->opcode) opl = sizeof f->opcode - 1;
    memcpy(f->opcode, b, opl); f->opcode[opl] = 0;
    for (size_t i = 0; f->opcode[i]; i++)
        f->opcode[i] = (char)tolower((unsigned char)f->opcode[i]);
    while (*sp && isspace((unsigned char)*sp)) sp++;

    static const struct { const char *w; int id; } PFX[] = {
        {"rep", 0}, {"repe", 1}, {"repz", 1}, {"repne", 2}, {"repnz", 2}, {"lock", 3}, {NULL, -1}
    };
    for (int i = 0; PFX[i].w; i++) {
        if (strcmp(f->opcode, PFX[i].w) == 0 && *sp) {
            f->prefix = PFX[i].id;
            char rest[512];
            strncpy(rest, sp, sizeof rest - 1); rest[sizeof rest - 1] = 0;
            char *q = rest;
            while (*q && !isspace((unsigned char)*q)) q++;
            size_t l = (size_t)(q - rest);
            if (l >= sizeof f->opcode) l = sizeof f->opcode - 1;
            memcpy(f->opcode, rest, l); f->opcode[l] = 0;
            while (*q && isspace((unsigned char)*q)) q++;
            g_strlcpy_compat(f->operands, q, sizeof f->operands);
            goto have_ops;
        }
    }
    g_strlcpy_compat(f->operands, sp, sizeof f->operands);

have_ops:
    {
        char clean[192];
        size_t o = 0; int lastsp = 0;
        for (const char *p = f->operands; *p; p++) {
            if (isspace((unsigned char)*p)) { if (!lastsp && o) clean[o++] = ' '; lastsp = 1; }
            else { clean[o++] = *p; lastsp = 0; }
            if (o + 1 >= sizeof clean) break;
        }
        clean[o] = 0;
        snprintf(f->operands, sizeof f->operands, "%s", clean);
        trim(f->operands);
    }
    extract_imm(f->operands, f->imm, sizeof f->imm);
}

/**
 * Возвращает строковое имя префикса по его коду.
 *
 * @param p  код префикса (0..3) или -1
 * @return   "rep"/"repe"/"repne"/"lock"/"-"
 */
static const char *prefix_name(int p)
{
    switch (p) {
        case 0: return "rep";
        case 1: return "repe";
        case 2: return "repne";
        case 3: return "lock";
        default: return "-";
    }
}

#include "bdtable.h"

static char bd_pool[8192];

/**
 * Разбирает многострочную таблицу (из engine_breakdown) на строки/колонки.
 * Строкой данных считается та, где >=5 символов '|'.
 *
 * @param table  текст таблицы
 * @param t      сюда кладутся строки и колонки
 * @return       число строк данных (0 если разбор не удался)
 */
int bd_parse(const char *table, BdTable *t)
{
    memset(t, 0, sizeof *t);
    size_t pp = 0;
    const char *p = table;
    while (*p && t->nrows < 64) {
        const char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        int bars = 0;
        for (size_t i = 0; i < ll; i++) if (p[i] == '|') bars++;
        if (bars >= 5 && ll + 1 < sizeof bd_pool - pp) {
            char *base = bd_pool + pp;
            memcpy(base, p, ll); base[ll] = 0;
            pp += ll + 1;
            char *cols[6] = {0};
            int nc = 0;
            char *q = base;
            while (nc < 5) {
                char *bar = strchr(q, '|');
                if (!bar) break;
                *bar = 0;
                cols[nc++] = q;
                q = bar + 1;
            }
            if (nc == 5) {
                cols[nc++] = q;
                for (int c = 0; c < 6; c++) {
                    char *v = cols[c];
                    while (*v == ' ' || *v == '\t') v++;
                    size_t L = strlen(v);
                    while (L && (v[L-1] == ' ' || v[L-1] == '\t')) v[--L] = 0;
                    t->row[t->nrows][c] = v;
                }
                t->nrows++;
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return t->nrows;
}

/**
 * Строит текстовую таблицу разбора кода (для GUI/CLI).
 * Для asm-входа сначала вызывает NASM, для hex-входа — parse_hex.
 *
 * @param src    исходник asm или hex
 * @param mode   режим
 * @param is_hex 1 если src — hex-байты, 0 если asm
 * @param out    буфер результата
 * @param cap    размер out
 * @param msg    сообщение об успехе или ошибке
 * @param msgcap размер msg
 * @return       0 при успехе, -1 при ошибке
 */
int engine_breakdown(const char *src, Mode mode, int is_hex,
                     char *out, size_t cap,
                     char *msg, size_t msgcap)
{
    if (ensure_tmpdir() != 0) {
        snprintf(msg, msgcap, "Нет каталога %s", TMPDIR);
        return -1;
    }
    unsigned char bytes[4096];
    size_t blen = 0;
    char hexbuf[2048] = "";

    if (is_hex) {
        if (parse_hex(src, bytes, sizeof bytes, &blen) != 0) {
            snprintf(msg, msgcap, "Не нашёл ни одного байта в hex-строке");
            return -1;
        }
    } else {
        char em[512];
        if (engine_assemble(src, mode, hexbuf, sizeof hexbuf, em, sizeof em) != 0) {
            snprintf(msg, msgcap, "%s", em);
            return -1;
        }
        parse_hex(hexbuf, bytes, sizeof bytes, &blen);
    }

    static char insns[64][256];
    static int sizes[64];
    int count = 0;
    char err[512];
    Mode used = auto_dis_mode(bytes, blen, mode);
    if (disasm_with_sizes(bytes, blen, used, insns, sizes, 64, &count, err, sizeof err) != 0) {
        snprintf(msg, msgcap, "%s", err);
        return -1;
    }

    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos,
        "%-3s | %-7s | %-8s | %-22s | %-10s | %-4s\n",
        "N", "Prefix", "Opcode", "Operands", "Immediate", "Size");
    pos += (size_t)snprintf(out + pos, cap - pos,
        "----+---------+----------+------------------------+------------+-----\n");
    for (int i = 0; i < count && pos < cap - 300; i++) {
        InsnFields f;
        insn_split(insns[i], used, &f);
        f.size = sizes[i];
        pos += (size_t)snprintf(out + pos, cap - pos,
            "%-3d | %-7s | %-8s | %-22s | %-10s | %-4d\n",
            i + 1, prefix_name(f.prefix), f.opcode,
            f.operands[0] ? f.operands : "-",
            f.imm[0] ? f.imm : "-", f.size);
    }

    snprintf(msg, msgcap, "%d инструкци%s · режим %s · всего %zu байт",
             count, (count == 1 ? "я" : (count < 5 ? "и" : "й")),
             mode_name[used], blen);
    return 0;
}

/**
 * Возвращает короткое русское пояснение к последней инструкции блока.
 *
 * @param line  текст блока (берётся последняя непустая строка)
 * @param out   буфер результата
 * @param cap   размер out
 */
void explain_line(const char *line, char *out, size_t cap)
{
    out[0] = 0;
    if (cap == 0) return;
    char b[256];
    strncpy(b, line, sizeof b - 1); b[sizeof b - 1] = 0;
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