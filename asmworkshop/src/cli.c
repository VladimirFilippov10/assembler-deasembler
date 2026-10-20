/* cli.c — ASM Workshop в терминале (тот же движок, без GUI).
 * Сборка: gcc -O2 -o asmworkshop_cli src/cli.c src/engine.c
 */
#include "core.h"
#include <stdio.h>
#include <string.h>
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

static int run_asm(const char *src, Mode m)
{
    char hex[2048], msg[512];
    int rc = engine_assemble(src, m, hex, sizeof hex, msg, sizeof msg);
    if (rc == 0) {
        printf("\nHEX : %s\n", hex);
        printf("ИНФО: %s\n", msg);
    } else printf("\nОШИБКА: %s\n", msg);
    return rc;
}

static int run_dis(const char *hx, Mode m)
{
    char asm_[2048], msg[512];
    int rc = engine_disassemble(hx, m, asm_, sizeof asm_, msg, sizeof msg);
    if (rc == 0) {
        printf("\nASM :\n%s\n", asm_);
        printf("ИНФО: %s\n", msg);
    } else printf("\nОШИБКА: %s\n", msg);
    return rc;
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
    printf("ASM Workshop CLI · режим com/16/32/64 · 'q' выход, '!hex'/'!asm' смена направления\n");
    int dir = 0; /* 0 = asm->hex, 1 = hex->asm */
    char line[4096];
    for (;;) {
        printf("%s> ", dir ? "hex→asm" : "asm→hex");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        size_t l = strlen(line);
        if (l && line[l-1] == '\n') line[--l] = 0;
        if (!strcmp(line, "q") || !strcmp(line, "quit")) break;
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
