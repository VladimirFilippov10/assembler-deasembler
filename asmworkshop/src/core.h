/* core.h — ASM Workshop: общий интерфейс GUI <-> движка
 * Минималистичный ассемблер/деасемблер (NASM + objdump), 16/32/64 бит.
 */
#ifndef CORE_H
#define CORE_H

#include <stddef.h>

typedef enum { MODE_COM = 0, MODE_16 = 1, MODE_32 = 2, MODE_64 = 3 } Mode;

typedef struct {
    char text[512];   /* сообщение для панели результатов */
    int  is_err;      /* 1 = ошибка */
    char copy[256];   /* строка для кнопки "Копировать" (hex или asm) */
} Result;

/* Собрать текст src (одна строка или блок NASM) в байты.
 * out_hex — hex-строка через пробел, out_msg — пояснение/ошибка. Возврат 0 при успехе. */
int engine_assemble(const char *src, Mode mode,
                    char *out_hex, size_t hexcap,
                    char *out_msg, size_t msgcap);

/* Деассемблировать hex-строку (байты вида "B8 01 00"). Возврат 0 при успехе. */
int engine_disassemble(const char *hexstr, Mode mode,
                       char *out_asm, size_t asmcap,
                       char *out_msg, size_t msgcap);

/* Короткое русское пояснение к инструкции (по первой мнемонике). */
void explain_line(const char *line, char *out, size_t cap);

#endif
