/* cli.c — ASM Workshop в терминале (тот же движок, без GUI).
 * Сборка: gcc -O2 -o asmworkshop_cli src/cli.c src/engine.c
 */
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

static void usage(const char *p)
{
    printf("Использование:\n");
    printf("  %s -a \"mov ax, 1\" [-m com|16|32|64]   ассемблировать строку\n", p);
    printf("  %s -d \"B8 01 00\"    [-m ...]          деассемблировать байты\n", p);
    printf("  %s -f file.asm      [-m ...]          собрать файл построчно\n", p);
    printf("  %s                                  интерактивный режим\n", p);
    printf("Режимы: com (DOSBox, ORG 0x100), 16, 32, 64. По умолчанию com.\n");
}

static Mode parse_mode(const char *s)
{
    if (!strcmp(s, "16")) return MODE_16;
    if (!strcmp(s, "32")) return MODE_32;
    if (!strcmp(s, "64")) return MODE_64;
    return MODE_COM;
}

/* Проверка, что строка похожа на корректную ассемблерную инструкцию. */
static int looks_like_instruction(const char *s)
{
    /* пропустить пробелы и метки ("label:") */
    while (*s == ' ' || *s == '\t') s++;
    const char *colon = strchr(s, ':');
    if (colon && colon != s) { /* метка — пропускаем её */
        s = colon + 1;
        while (*s == ' ' || *s == '\t') s++;
    }
    if (!*s) return 0;
    /* первый токен должен быть мнемоникой из букв */
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) return 0;
    for (const char *p = s; ; p++) {
        if (*p == ',' || *p == ' ' || *p == '\t' || *p == 0) break;
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    }
    return 1;
}

/* Убрать вывод NASM с путём /tmp/... — оставить только текст ошибки. */
static void clean_err(char *msg, size_t cap)
{
    char *nl = strchr(msg, '\n');
    if (nl) *nl = 0; /* первая строка ошибки */
    char *err = strstr(msg, "error:");
    if (err) {
        /* обрезать путь вида "...asm:4: error: ..." -> "error: ..." */
        memmove(msg, err, strlen(err) + 1);
    }
    snprintf(msg + strlen(msg), cap - strlen(msg),
             "\nПодсказка: вводите инструкцию, например 'mov ax, 1'. 'help' — список команд.");
}

static int run_asm(const char *src, Mode m)
{
    if (!looks_like_instruction(src)) {
        printf("\nОШИБКА: '%s' не похоже на инструкцию.\n"
               "Подсказка: вводите код без пути к файлу, например 'mov ax, 1'.\n"
               "Для списка команд введите 'help'.\n", src);
        return 1;
    }
    char hex[2048], msg[512];
    int rc = engine_assemble(src, m, hex, sizeof hex, msg, sizeof msg);
    if (rc == 0) {
        printf("\nHEX : %s\n", hex);
        printf("ИНФО: %s\n", msg);
    } else {
        clean_err(msg, sizeof msg);
        printf("\nОШИБКА: %s\n", msg);
    }
    return rc;
}

static int run_dis(const char *hx, Mode m)
{
    /* проверка: только hex-байты и пробелы */
    for (const char *p = hx; *p; p++) {
        if (*p == ' ' || *p == '\t') continue;
        if (!isxdigit((unsigned char)*p)) {
            printf("\nОШИБКА: '%s' не является HEX-строкой (ожидается, напр., 'B8 01 00').\n"
                   "Введите '!hex', чтобы переключиться в режим asm→hex.\n", hx);
            return 1;
        }
    }
    char asm_[2048], msg[512];
    int rc = engine_disassemble(hx, m, asm_, sizeof asm_, msg, sizeof msg);
    if (rc == 0) {
        printf("\nASM :\n%s\n", asm_);
        printf("ИНФО: %s\n", msg);
    } else {
        clean_err(msg, sizeof msg);
        printf("\nОШИБКА: %s\n", msg);
    }
    return rc;
}

static void print_help(void)
{
    printf("\nКоманды интерактивного режима:\n");
    printf("  q / quit      выход\n");
    printf("  help          эта справка\n");
    printf("  !hex          направление asm -> hex (вводите инструкции)\n");
    printf("  !asm          направление hex -> asm (вводите байты, напр. B8 01 00)\n");
    printf("  !com/!16/!32/!64  смена режима\n");
    printf("  clear         очистить экран\n");
    printf("Примеры:\n");
    printf("  asm→hex> mov ax, 1   ->  HEX : B8 01 00\n");
    printf("  hex→asm> B8 01 00    ->  ASM : mov ax, 1\n\n");
}

int main(int argc, char **argv)
{
    Mode mode = MODE_COM;
    const char *asmsrc = NULL, *hexsrc = NULL, *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) mode = parse_mode(argv[++i]);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) asmsrc = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) hexsrc = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
    }

    if (asmsrc) return run_asm(asmsrc, mode) ? 1 : 0;
    if (hexsrc) return run_dis(hexsrc, mode) ? 1 : 0;
    if (file) {
        char buf[32768];
        FILE *f = fopen(file, "rb");
        if (!f) { perror(file); return 1; }
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0; fclose(f);
        return run_asm(buf, mode) ? 1 : 0;
    }

    /* интерактив */
    printf("ASM Workshop CLI · режим com/16/32/64 · 'q' выход, '!hex'/'!asm' смена направления, 'help' справка\n");
    int dir = 0; /* 0 = asm->hex, 1 = hex->asm */
    char line[4096];
    for (;;) {
        printf("%s> ", dir ? "hex→asm" : "asm→hex");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        size_t l = strlen(line);
        if (l && line[l-1] == '\n') line[--l] = 0;
        if (!strcmp(line, "q") || !strcmp(line, "quit")) break;
        if (!strcmp(line, "help")) { print_help(); continue; }
        if (!strcmp(line, "clear")) { printf("\033[2J\033[H"); continue; }
        if (!strcmp(line, "!hex")) { dir = 0; continue; }
        if (!strcmp(line, "!asm")) { dir = 1; continue; }
        if (!strcmp(line, "!16")) { mode = MODE_16; continue; }
        if (!strcmp(line, "!32")) { mode = MODE_32; continue; }
        if (!strcmp(line, "!64")) { mode = MODE_64; continue; }
        if (!strcmp(line, "!com")) { mode = MODE_COM; continue; }
        if (!l) continue;
        if (dir) run_dis(line, mode); else run_asm(line, mode);
    }
    return 0;
}
