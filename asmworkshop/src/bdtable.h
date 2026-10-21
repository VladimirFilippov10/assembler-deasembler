/* bdtable.h — разбор текста таблицы (из engine_breakdown) на строки/колонки.
 * Общая для GUI (GtkTreeView) и CLI (ASCII-таблица). Без зависимостей от GTK.
 */
#ifndef BDTABLE_H
#define BDTABLE_H

typedef struct {
    const char *head[6];   /* заголовки полей */
    const char *row[64][6];/* данные: до 64 строк по 6 колонок */
    int nrows;
} BdTable;

/* Разобрать многострочный текст таблицы вида "a | b | c | d | e | f".
 * Возвращает число строк данных (0 — если разбор не удался). */
int bd_parse(const char *table, BdTable *t);

#endif
