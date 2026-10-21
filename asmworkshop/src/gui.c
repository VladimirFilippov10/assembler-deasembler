/* gui.c — ASM Workshop: бело-синий интерфейс (GTK3, чистый C).
 * Моноширинный ввод/вывод, режимы COM16/16/32/64, кнопки копирования,
 * таблица разбора команды (по полям) через GtkTreeView.
 */
#include "core.h"
#include "theme.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

static GtkWidget *win;
static GtkWidget *mode_combo;
static GtkWidget *asm_in, *hex_in;      /* текстовые поля */
static GtkWidget *asm_out, *hex_out;    /* результаты */
static GtkWidget *asm_note, *hex_note;  /* пояснения */
static GtkWidget *statusbar;

/* ─────────────── стиль (палитра в theme.h) ─────────────── */

#define OK_COLOR   "#15803d"
#define ERR_COLOR  "#b91c1c"
#define MUTED      "#5b7ba6"

static Mode current_mode(void)
{
    switch (gtk_combo_box_get_active(GTK_COMBO_BOX(mode_combo))) {
        case 1:  return MODE_16;
        case 2:  return MODE_32;
        case 3:  return MODE_64;
        default: return MODE_COM;
    }
}

static void set_buf(GtkWidget *tv, const char *s)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(b, s ? s : "", -1);
}

static char *get_buf(GtkWidget *tv)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GtkTextIter i0, i1;
    gtk_text_buffer_get_bounds(b, &i0, &i1);
    return gtk_text_buffer_get_text(b, &i0, &i1, FALSE);
}

static void show_result(GtkWidget *note_lbl, GtkWidget *out_tv,
                        const char *msg, const char *body, int is_err)
{
    set_buf(out_tv, body);
    /* note_lbl показывает статус + первое сообщение */
    GString *s = g_string_new(NULL);
    g_string_append_printf(s,
        "<span foreground='%s' weight='bold'>%s</span>\n",
        is_err ? ERR_COLOR : OK_COLOR, is_err ? "✕ ошибка" : "✓ готово");
    /* все строки msg мелким шрифтом */
    char **lines = g_strsplit(msg, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *esc = g_markup_escape_text(lines[i], -1);
        g_string_append_printf(s, "<span size='9000' foreground='%s'>%s</span>\n",
                               is_err ? ERR_COLOR : MUTED, esc);
        g_free(esc);
    }
    g_strfreev(lines);
    gtk_label_set_markup(GTK_LABEL(note_lbl), s->str);
    g_string_free(s, TRUE);
}

/* ─────────────── таблица разбора команды ─────────────── */

enum { COL_N, COL_PFX, COL_OP, COL_OPS, COL_IMM, COL_SIZE, N_COLS };

static GtkListStore *lbd_store;  /* карточка ассемблирования */
static GtkListStore *rbd_store;  /* карточка деассемблирования */

static GtkWidget *make_breakdown_tree(const char *title, GtkListStore **store_out)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), title);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "cardtitle");
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);

    GtkListStore *store = gtk_list_store_new(N_COLS,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    *store_out = store;

    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(tree), GTK_TREE_VIEW_GRID_LINES_BOTH);

    const char *heads[] = { "№", "Префикс", "Opcode", "Операнды", "Immediate", "Размер, байт" };
    for (int i = 0; i < N_COLS; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        g_object_set(r, "family", "monospace", NULL);
        if (i == COL_N || i == COL_SIZE)
            g_object_set(r, "xalign", 0.5f, NULL);
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(
            heads[i], r, "text", i, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        if (i == COL_OPS) gtk_tree_view_column_set_expand(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
    }

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), tree);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    return box;
}

/* Заполнить таблицу из текста разбора: строка данных = "a | b | c | d | e | f". */
static void fill_breakdown(GtkListStore *store, const char *table)
{
    gtk_list_store_clear(store);
    char **lines = g_strsplit(table, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *ln = lines[i];
        if (!ln[0]) continue;
        if (strstr(ln, "----+")) continue;                 /* разделитель */
        if (strstr(ln, "Opcode") && strstr(ln, "Operands")) continue; /* заголовок */
        int bars = 0;
        for (char *q = ln; *q; q++) if (*q == '|') bars++;
        if (bars < N_COLS - 1) continue;                   /* не строка данных */
        /* разрезать по '|' */
        char *cols[N_COLS] = {0};
        int ncol = 0;
        char *p = ln;
        while (ncol < N_COLS - 1) {
            char *bar = strchr(p, '|');
            if (!bar) break;
            *bar = 0;
            cols[ncol++] = p;
            p = bar + 1;
        }
        cols[ncol++] = p;
        GtkTreeIter it;
        gtk_list_store_append(store, &it);
        for (int c = 0; c < N_COLS; c++) {
            char buf[256];
            const char *v = cols[c] ? cols[c] : "";
            while (*v == ' ') v++;
            snprintf(buf, sizeof buf, "%s", v);
            size_t L = strlen(buf);
            while (L && buf[L-1] == ' ') buf[--L] = 0;
            gtk_list_store_set(store, &it, c, buf, -1);
        }
    }
    g_strfreev(lines);
}

/* ─────────────── обработчики ─────────────── */

static void do_assemble(void)
{
    char *src = get_buf(asm_in);
    char hex[2048], msg[512];
    int rc = engine_assemble(src, current_mode(), hex, sizeof hex, msg, sizeof msg);
    show_result(asm_note, asm_out, msg, rc == 0 ? hex : "", rc != 0);
    /* таблица разбора по полям */
    if (lbd_store) {
        if (rc == 0) {
            char table[8192], tmsg[512];
            if (engine_breakdown(src, current_mode(), 0, table, sizeof table,
                                 tmsg, sizeof tmsg) == 0)
                fill_breakdown(lbd_store, table);
            else gtk_list_store_clear(lbd_store);
        } else gtk_list_store_clear(lbd_store);
    }
    g_free(src);
}

static void on_assemble(GtkButton *btn, gpointer u)
{
    (void)btn; (void)u;
    do_assemble();
}

static void do_disassemble(void)
{
    char *hx = get_buf(hex_in);
    char asm_[2048], msg[512];
    int rc = engine_disassemble(hx, current_mode(), asm_, sizeof asm_, msg, sizeof msg);
    show_result(hex_note, hex_out, msg, rc == 0 ? asm_ : "", rc != 0);
    if (rbd_store) {
        if (rc == 0) {
            char table[8192], tmsg[512];
            if (engine_breakdown(hx, current_mode(), 1, table, sizeof table,
                                 tmsg, sizeof tmsg) == 0)
                fill_breakdown(rbd_store, table);
            else gtk_list_store_clear(rbd_store);
        } else gtk_list_store_clear(rbd_store);
    }
    g_free(hx);
}

static void on_disassemble(GtkButton *btn, gpointer u)
{
    (void)btn; (void)u;
    do_disassemble();
}

/* Ctrl+Enter в любом текстовом поле запускает соответствующую сборку/разбор */
static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer user_data)
{
    (void)w;
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_Return ||
                                           ev->keyval == GDK_KEY_KP_Enter)) {
        if (user_data == GINT_TO_POINTER(0)) do_assemble();
        else do_disassemble();
        return TRUE;
    }
    return FALSE;
}

static void copy_to_clip(const char *text)
{
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, text, -1);
}

static char *buf_line(GtkWidget *tv)
{
    char *t = get_buf(tv);
    /* склеить многострочное в одну строку для копирования hex */
    for (char *p = t; *p; p++) if (*p == '\n') *p = ' ';
    return t;
}

static void on_copy_asm_hex(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    char *t = buf_line(asm_out);
    copy_to_clip(t);
    g_free(t);
}
static void on_copy_hex_asm(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    char *t = get_buf(hex_out);
    copy_to_clip(t);
    g_free(t);
}

static void on_clear(GtkButton *b, gpointer which)
{
    (void)b;
    if (which == GINT_TO_POINTER(0)) {
        set_buf(asm_in, ""); set_buf(asm_out, "");
        gtk_label_set_text(GTK_LABEL(asm_note), "");
    } else {
        set_buf(hex_in, ""); set_buf(hex_out, "");
        gtk_label_set_text(GTK_LABEL(hex_note), "");
    }
}

static void on_paste_here(GtkButton *b, gpointer tv)
{
    (void)b;
    GtkWidget *view = GTK_WIDGET(tv);
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    char *t = gtk_clipboard_wait_for_text(cb);
    if (t) { set_buf(view, t); g_free(t); }
}

static void on_swap(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    /* результат слева -> поле справа (быстрый круг asm->hex->asm) */
    char *t = buf_line(asm_out);
    if (t && *t) set_buf(hex_in, t);
    g_free(t);
}

/* ─────────────── построение интерфейса ─────────────── */

static GtkWidget* make_card(const char *title, GtkWidget **content_slot)
{
    GtkWidget *outer = gtk_event_box_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(outer), "card");
    gtk_widget_set_margin_start(outer, 10);
    gtk_widget_set_margin_end(outer, 10);
    gtk_widget_set_margin_top(outer, 10);
    gtk_widget_set_margin_bottom(outer, 10);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_container_add(GTK_CONTAINER(outer), box);

    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), title);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "cardtitle");
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    *content_slot = box;
    return outer;
}

static GtkWidget* make_textview(gboolean editable, gboolean mono_out)
{
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(tv), 8);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), editable);
    if (!editable && mono_out)
        gtk_style_context_add_class(gtk_widget_get_style_context(tv), "out");
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 150);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_NONE);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    return sw; /* возвращаем scrolled window; сам tv достанем ниже */
}

/* хелпер: найти textview внутри scrolledwindow */
static GtkWidget* tv_of(GtkWidget *sw)
{
    return gtk_bin_get_child(GTK_BIN(sw));
}

static GtkWidget* small_btn(const char *label, const char *css_class,
                            GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    if (css_class)
        gtk_style_context_add_class(gtk_widget_get_style_context(b), css_class);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    /* глобальный CSS */
    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, THEME_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "ASM Workshop — ассемблер ⇄ деасемблер");
    gtk_window_set_default_size(GTK_WINDOW(win), 1020, 640);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), root);

    /* ── шапка ── */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(head), 16);
    gtk_box_pack_start(GTK_BOX(root), head, FALSE, FALSE, 0);

    GtkWidget *brand = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(brand),
        "<span size='20000' weight='bold' foreground='#1d4ed8'>⌁ ASM WORKSHOP</span>"
        "\n<span size='9000' foreground='#5b7ba6'>ассемблер ⇄ деасемблер · NASM · Intel · DOSBox COM</span>");
    gtk_widget_set_halign(brand, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(head), brand, TRUE, TRUE, 0);

    GtkWidget *modelbl = gtk_label_new("режим:");
    gtk_box_pack_start(GTK_BOX(head), modelbl, FALSE, FALSE, 0);
    mode_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode_combo), "COM 16-bit (DOSBox, ORG 0x100)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode_combo), "16-bit raw");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode_combo), "32-bit raw");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode_combo), "64-bit raw");
    gtk_combo_box_set_active(GTK_COMBO_BOX(mode_combo), 0);
    gtk_box_pack_start(GTK_BOX(head), mode_combo, FALSE, FALSE, 0);

    /* ── две колонки ── */
    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(root), cols, TRUE, TRUE, 0);

    /* левая: ассемблирование */
    GtkWidget *lcontent;
    GtkWidget *lcard = make_card("АССЕМБЛИРОВАНИЕ   ASM → HEX", &lcontent);
    gtk_box_pack_start(GTK_BOX(cols), lcard, TRUE, TRUE, 0);

    GtkWidget *l_sw_in = make_textview(TRUE, FALSE);
    asm_in = tv_of(l_sw_in);
    gtk_box_pack_start(GTK_BOX(lcontent), l_sw_in, TRUE, TRUE, 0);

    GtkWidget *lbtns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(lbtns),
        small_btn("Собрать →", "primary", G_CALLBACK(on_assemble), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lbtns),
        small_btn("Копировать HEX", NULL, G_CALLBACK(on_copy_asm_hex), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lbtns),
        small_btn("Вставить", "flat", G_CALLBACK(on_paste_here), asm_in), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(lbtns),
        small_btn("Очистить", "flat", G_CALLBACK(on_clear), GINT_TO_POINTER(0)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lcontent), lbtns, FALSE, FALSE, 0);

    GtkWidget *l_sw_out = make_textview(FALSE, TRUE);
    asm_out = tv_of(l_sw_out);
    gtk_box_pack_start(GTK_BOX(lcontent), l_sw_out, TRUE, TRUE, 0);

    asm_note = gtk_label_new("");
    gtk_label_set_use_markup(GTK_LABEL(asm_note), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(asm_note), TRUE);
    gtk_widget_set_halign(asm_note, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(lcontent), asm_note, FALSE, FALSE, 0);

    /* таблица разбора команды по полям */
    GtkWidget *lbd = make_breakdown_tree("РАЗБОР КОМАНДЫ ПО ПОЛЯМ", &lbd_store);
    gtk_box_pack_start(GTK_BOX(lcontent), lbd, TRUE, TRUE, 0);

    /* правая: деассемблирование */
    GtkWidget *rcontent;
    GtkWidget *rcard = make_card("ДЕАССЕМБЛИРОВАНИЕ   HEX → ASM", &rcontent);
    gtk_box_pack_start(GTK_BOX(cols), rcard, TRUE, TRUE, 0);

    GtkWidget *r_sw_in = make_textview(TRUE, FALSE);
    hex_in = tv_of(r_sw_in);
    gtk_box_pack_start(GTK_BOX(rcontent), r_sw_in, TRUE, TRUE, 0);

    GtkWidget *rbtns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(rbtns),
        small_btn("← Разобрать", "primary", G_CALLBACK(on_disassemble), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rbtns),
        small_btn("Копировать ASM", NULL, G_CALLBACK(on_copy_hex_asm), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rbtns),
        small_btn("Вставить", "flat", G_CALLBACK(on_paste_here), hex_in), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(rbtns),
        small_btn("Очистить", "flat", G_CALLBACK(on_clear), GINT_TO_POINTER(1)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rcontent), rbtns, FALSE, FALSE, 0);

    GtkWidget *r_sw_out = make_textview(FALSE, TRUE);
    hex_out = tv_of(r_sw_out);
    gtk_box_pack_start(GTK_BOX(rcontent), r_sw_out, TRUE, TRUE, 0);

    hex_note = gtk_label_new("");
    gtk_label_set_use_markup(GTK_LABEL(hex_note), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(hex_note), TRUE);
    gtk_widget_set_halign(hex_note, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(rcontent), hex_note, FALSE, FALSE, 0);

    /* таблица разбора команды по полям */
    GtkWidget *rbd = make_breakdown_tree("РАЗБОР КОМАНДЫ ПО ПОЛЯМ", &rbd_store);
    gtk_box_pack_start(GTK_BOX(rcontent), rbd, TRUE, TRUE, 0);

    /* ── нижняя строка состояния ── */
    statusbar = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(statusbar),
        "<span foreground='#5b7ba6'>nasm + objdump · введите команду (mov ax, 1) или байты (B8 01 00) · Ctrl+Enter = собрать · Ctrl+R = разбор по полям</span>");
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(foot), 10);
    gtk_box_pack_start(GTK_BOX(foot), statusbar, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(foot),
        small_btn("результат → поле HEX-ввода", "flat", G_CALLBACK(on_swap), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), foot, FALSE, FALSE, 0);

    /* горячие клавиши Ctrl+Enter */
    g_signal_connect(asm_in, "key-press-event", G_CALLBACK(on_key_press), GINT_TO_POINTER(0));
    g_signal_connect(hex_in, "key-press-event", G_CALLBACK(on_key_press), GINT_TO_POINTER(1));

    /* стартовые примеры */
    set_buf(asm_in, "mov ax, 0x4C00\nint 0x21");
    set_buf(hex_in, "B8 01 00 00 00");

    gtk_widget_show_all(win);
    /* показать разбор полей для стартовых примеров */
    do_assemble();
    do_disassemble();
    gtk_main();
    return 0;
}
