/* gui_win.c — ASM Workshop: нативное GUI на чистом Win32 API (без Python, без GTK).
 * Тёмная тема, два блока (ASM→HEX и HEX→ASM), ТАБЛИЦЫ результатов, кнопка «О программе».
 * Сборка: asmworkshop\build.bat (MinGW-w64 gcc или MSVC cl)
 */
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <direct.h>

#include "core.h"

#define APP_TITLE_W    L"ASM Workshop — ассемблер ⇄ деасемблер"
#define APP_VERSION    "v1.0 (чистый C / Win32)"

/* ─────────── идентификаторы ─────────── */
enum {
    IDC_MODE = 101,
    IDC_ASM_IN = 110, IDC_ASM_BTN = 111, IDC_ASM_HEXOUT = 112,
    IDC_ASM_GRID = 113, IDC_ASM_MSG = 114, IDC_ASM_COPY = 115,
    IDC_HEX_IN = 120, IDC_HEX_BTN = 121, IDC_HEX_OUT = 122,
    IDC_HEX_GRID = 123, IDC_HEX_MSG = 124, IDC_HEX_COPY = 125,
    IDC_ABOUT_BTN = 130, IDC_CLEAR_L = 131, IDC_CLEAR_R = 132,
    IDC_SWAP_BTN = 133,
};

/* ─────────── цвета тёмной темы ─────────── */
#define CLR_BG     RGB(0x14,0x16,0x1A)
#define CLR_CARD   RGB(0x1B,0x1E,0x24)
#define CLR_EDIT   RGB(0x16,0x19,0x1F)
#define CLR_TEXT   RGB(0xE8,0xEA,0xF0)
#define CLR_NOTE   RGB(0x9A,0xA4,0xBA)
#define CLR_MUTED  RGB(0x8B,0x93,0xA7)
#define CLR_BORDER RGB(0x26,0x2A,0x33)

/* ─────────── глобальные виджеты ─────────── */
static HWND g_hMain, g_hMode;
static HWND g_hAsmIn, g_hAsmBtn, g_hAsmHexOut, g_hAsmGrid, g_hAsmMsg, g_hAsmCopy;
static HWND g_hHexIn, g_hHexBtn, g_hHexOut, g_hHexGrid, g_hHexMsg, g_hHexCopy;
static HWND g_hAboutBtn, g_hClearL, g_hClearR, g_hSwapBtn, g_hStatus;
static HFONT g_fUi, g_fMono, g_fBrand, g_fCardTitle;
static HBRUSH g_brBg, g_brCard, g_brEdit;
static char g_last_hex[2048] = "";
static char g_last_asm[2048] = "";

/* ─────────── utf8 <-> wide ─────────── */
static void u2w(const char *s, WCHAR *w, int cap) { MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap); }
static void set_utf8(HWND h, const char *s)
{
    static WCHAR w[8192];
    u2w(s ? s : "", w, (int)(sizeof w / sizeof w[0]));
    SetWindowTextW(h, w);
}
static void get_utf8(HWND h, char *out, size_t cap)
{
    out[0] = 0;
    int wl = GetWindowTextLengthW(h);
    if (wl <= 0 || cap == 0) return;
    WCHAR *w = (WCHAR *)malloc((size_t)(wl + 1) * sizeof(WCHAR));
    if (!w) return;
    GetWindowTextW(h, w, wl + 1);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    out[cap - 1] = 0;
    free(w);
}

static Mode current_mode(void)
{
    switch ((int)SendMessageW(g_hMode, CB_GETCURSEL, 0, 0)) {
        case 1: return MODE_16;
        case 2: return MODE_32;
        case 3: return MODE_64;
        default: return MODE_COM;
    }
}
static const char *mode_label(Mode m)
{
    switch (m) {
        case MODE_16: return "16-bit";
        case MODE_32: return "32-bit";
        case MODE_64: return "64-bit";
        default: return "COM 16-bit";
    }
}

/* ─────────── разбор hex-строки в байты ─────────── */
static size_t hexstr_to_bytes(const char *hx, unsigned char *b, size_t cap)
{
    size_t n = 0;
    const char *p = hx;
    while (*p && n < cap) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == ',' || *p == ';' || *p == 'x' || *p == 'X') { p++; continue; }
        if (!isxdigit((unsigned char)*p)) { p++; continue; }
        char pair[3] = {0,0,0};
        int cnt = 0;
        while (*p && isxdigit((unsigned char)*p) && cnt < 2) pair[cnt++] = *p++;
        if (cnt == 1) { pair[1] = pair[0]; pair[0] = '0'; }
        b[n++] = (unsigned char)strtoul(pair, NULL, 16);
    }
    return n;
}

/* убрать \r */
static void strip_cr(char *s)
{
    char *r = s, *w = s;
    while (*r) { if (*r != '\r') *w++ = *r; r++; }
    *w = 0;
}

/* ─────────── данные таблиц ─────────── */
#define MAXROWS 512
typedef struct {
    int rows;
    char c[MAXROWS][4][40]; /* строки × 4 колонки (utf-8) */
} TableData;
static TableData g_td_asm, g_td_hex;

/* Собственный лёгкий подкласс (без SetWindowSubclass — доступен только в comctl32 v6):
 * сохраняем исходную wndproc через атрибут окна. */
static LRESULT CALLBACK GridProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NOTIFY) {
        LPNMHDR nh = (LPNMHDR)l;
        if (nh->code == LVN_GETDISPINFOW) {
            NMLVDISPINFOW *di = (NMLVDISPINFOW *)l;
            TableData *td = (TableData *)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (td && di->item.iItem >= 0 && di->item.iItem < td->rows &&
                (di->item.mask & LVIF_TEXT) &&
                di->item.iSubItem >= 0 && di->item.iSubItem < 4) {
                u2w(td->c[di->item.iItem][di->item.iSubItem],
                    di->item.pszText, (int)di->item.cchTextMax);
            }
            return 0;
        }
    }
    if (m == WM_NCDESTROY) RemovePropW(h, L"oldproc");
    WNDPROC old = (WNDPROC)GetPropW(h, L"oldproc");
    if (!old) old = DefWindowProcW;
    return CallWindowProcW(old, h, m, w, l);
}

static void table_fill(HWND hList, TableData *td, int rows)
{
    if (rows > MAXROWS) rows = MAXROWS;
    td->rows = rows;
    SetWindowLongPtrW(hList, GWLP_USERDATA, (LONG_PTR)td);
    SendMessageW(hList, LVM_SETITEMCOUNT, (WPARAM)rows, 0);
    InvalidateRect(hList, NULL, TRUE);
    UpdateWindow(hList);
}

/* ─────────── пояснение к первому байту ─────────── */
static void explain_first_byte(unsigned char b, char *out, size_t cap)
{
    struct { unsigned char pre, mask; const char *txt; } T[] = {
        {0x90,0xFF,"nop — нет операции"},
        {0xB0,0xF0,"mov r8,imm8 — константа в регистр"},
        {0xB8,0xF8,"mov reg,imm — загрузка константы"},
        {0xC3,0xFF,"ret — возврат из процедуры"},
        {0xCC,0xFF,"int3 — трассировочное прерывание"},
        {0xCD,0xFF,"int imm8 — прерывание (DOS 0x21)"},
        {0xE8,0xFF,"call rel — вызов процедуры"},
        {0xE9,0xFF,"jmp rel — безусловный переход"},
        {0xEB,0xFF,"jmp short — короткий переход"},
        {0x50,0xF8,"push — положить на стек"},
        {0x58,0xF8,"pop — снять со стека"},
        {0xF4,0xFF,"hlt — останов процессора"},
        {0xF8,0xFF,"clc — сброс флага переноса"},
        {0xFA,0xFF,"cli — запрет прерываний"},
        {0xFB,0xFF,"sti — разрешить прерывания"},
        {0xFC,0xFF,"cld — DF=0"},
        {0xFD,0xFF,"std — DF=1"},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if ((b & T[i].mask) == T[i].pre) { snprintf(out, cap, "%s", T[i].txt); return; }
    snprintf(out, cap, "байт кода 0x%02X", b);
}

/* ─────────── сборка таблиц ─────────── */
static void fill_asm_table(const unsigned char *bytes, size_t len, Mode mode, long org)
{
    long addr = org;
    int shown = (int)(len < MAXROWS ? len : MAXROWS);
    for (int i = 0; i < shown; i++) {
        snprintf(g_td_asm.c[i][0], 40, "0x%04lX", (unsigned long)addr++);
        snprintf(g_td_asm.c[i][1], 40, "%02X", bytes[i]);
        if (i == 0) explain_first_byte(bytes[0], g_td_asm.c[i][2], 40);
        else if (i < 8) strcpy(g_td_asm.c[i][2], "опкод/операнд");
        else strcpy(g_td_asm.c[i][2], "...");
        snprintf(g_td_asm.c[i][3], 40, "%s", mode_label(mode));
    }
    table_fill(g_hAsmGrid, &g_td_asm, shown);
}

static void fill_hex_table(const char *asmtext, size_t blen, Mode mode)
{
    static char buf[2048];
    strncpy(buf, asmtext, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    strip_cr(buf);
    char *lines[MAXROWS]; int rows = 0;
    char *p = buf;
    lines[rows++] = p;
    while (*p && rows < MAXROWS) {
        if (*p == '\n') { *p = 0; if (p[1]) lines[rows++] = p + 1; }
        p++;
    }
    size_t off = 0;
    size_t per = rows > 0 ? blen / (size_t)rows : 1;
    size_t rem = rows > 0 ? blen % (size_t)rows : 0;
    for (int i = 0; i < rows; i++) {
        size_t ln = per;
        if (rem > 0) { ln++; rem--; }
        if (ln == 0) ln = 1;
        snprintf(g_td_hex.c[i][0], 40, "0x%04zX", off);
        snprintf(g_td_hex.c[i][1], 40, "%d", i + 1);
        snprintf(g_td_hex.c[i][2], 40, "%.38s", lines[i]);
        snprintf(g_td_hex.c[i][3], 40, "%u байт · %s", (unsigned)ln, mode_label(mode));
        off += ln;
    }
    table_fill(g_hHexGrid, &g_td_hex, rows);
}

/* ─────────── действия ─────────── */
static void do_assemble(void)
{
    char src[8192], hex[2048], msg[512];
    get_utf8(g_hAsmIn, src, sizeof src);
    strip_cr(src);
    if (!src[0]) { set_utf8(g_hAsmMsg, "Введите код ассемблера."); return; }
    Mode m = current_mode();
    int rc = engine_assemble(src, m, hex, sizeof hex, msg, sizeof msg);
    char full[4096];
    if (rc == 0) {
        set_utf8(g_hAsmHexOut, hex);
        strncpy(g_last_hex, hex, sizeof g_last_hex - 1);
        g_last_hex[sizeof g_last_hex - 1] = 0;
        snprintf(full, sizeof full, "OK · %s", msg);
        set_utf8(g_hAsmMsg, full);
        unsigned char bytes[1024];
        size_t len = hexstr_to_bytes(hex, bytes, sizeof bytes);
        fill_asm_table(bytes, len, m, m == MODE_COM ? 0x100 : 0);
    } else {
        g_last_hex[0] = 0;
        set_utf8(g_hAsmHexOut, "");
        snprintf(full, sizeof full, "ERROR · %s", msg);
        set_utf8(g_hAsmMsg, full);
        table_fill(g_hAsmGrid, &g_td_asm, 0);
    }
}

static void do_disassemble(void)
{
    char hx[4096], asmout[2048], msg[512];
    get_utf8(g_hHexIn, hx, sizeof hx);
    strip_cr(hx);
    if (!hx[0]) { set_utf8(g_hHexMsg, "Введите hex-байты."); return; }
    Mode m = current_mode();
    int rc = engine_disassemble(hx, m, asmout, sizeof asmout, msg, sizeof msg);
    char full[4096];
    if (rc == 0) {
        set_utf8(g_hHexOut, asmout);
        strncpy(g_last_asm, asmout, sizeof g_last_asm - 1);
        g_last_asm[sizeof g_last_asm - 1] = 0;
        snprintf(full, sizeof full, "OK · %s", msg);
        set_utf8(g_hHexMsg, full);
        unsigned char bytes[1024];
        size_t len = hexstr_to_bytes(hx, bytes, sizeof bytes);
        fill_hex_table(asmout, len, m);
    } else {
        g_last_asm[0] = 0;
        set_utf8(g_hHexOut, "");
        snprintf(full, sizeof full, "ERROR · %s", msg);
        set_utf8(g_hHexMsg, full);
        table_fill(g_hHexGrid, &g_td_hex, 0);
    }
}

static void copy_text(const char *s)
{
    if (!s || !*s) return;
    if (!OpenClipboard(g_hMain)) return;
    EmptyClipboard();
    int wl = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (size_t)wl * sizeof(WCHAR));
    if (hg) {
        WCHAR *p = (WCHAR *)GlobalLock(hg);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, p, wl);
        GlobalUnlock(hg);
        SetClipboardData(CF_UNICODETEXT, hg);
    }
    CloseClipboard();
}

/* ─────────── о программе ─────────── */
static void show_about(HWND parent)
{
    static const char *TXT =
        "ASM WORKSHOP  " APP_VERSION "\r\n\r\n"
        "Минималистичный ассемблер/деасемблер синтаксиса Intel (NASM).\r\n"
        "Написан на чистом C (Win32 API) — без Python и интерпретаторов.\r\n\r\n"
        "Режимы:\r\n"
        "  COM 16-bit — для DOSBox debug.exe (ORG 0x100)\r\n"
        "  16 / 32 / 64-bit raw — голые байты\r\n\r\n"
        "Как пользоваться:\r\n"
        "  1. Введите инструкцию (mov ax, 1) и нажмите \"Собрать\".\r\n"
        "  2. Получите HEX и таблицу байтов; скопируйте в .COM-файл.\r\n"
        "  3. Или вставьте байты (B8 01 00) и нажмите \"Разобрать\".\r\n"
        "  4. Ctrl+Enter запускает сборку/разбор из активного поля.\r\n\r\n"
        "Требуется (рядом с exe или в PATH):\r\n"
        "  nasm.exe    — ассемблирование\r\n"
        "  objdump.exe — деассемблирование\r\n";
    MessageBoxA(parent, TXT, "О программе — ASM Workshop", MB_OK | MB_ICONINFORMATION);
}

/* ─────────── создание контролов ─────────── */
static HWND mk_edit(HWND parent, int id, int x, int y, int cx, int cy, DWORD extra)
{
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
        ES_WANTRETURN | extra,
        x, y, cx, cy, parent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
}
static HWND mk_btn(HWND parent, int id, const wchar_t *label, int x, int y, int cx, int cy, BOOL primary)
{
    HWND h = CreateWindowExW(0, L"BUTTON", label,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        x, y, cx, cy, parent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
    if (primary) SetPropW(h, L"prim", (HANDLE)TRUE);
    return h;
}
static HWND mk_static(HWND parent, const wchar_t *label, int x, int y, int cx, int cy)
{
    return CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        x, y, cx, cy, parent, NULL, GetModuleHandle(NULL), NULL);
}
static HWND mk_grid(HWND parent, int id, int x, int y, int cx, int cy,
                    const wchar_t *c0, const wchar_t *c1, const wchar_t *c2, const wchar_t *c3)
{
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
        x, y, cx, cy, parent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
    ListView_SetExtendedListViewStyle(h, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW lc; ZeroMemory(&lc, sizeof lc);
    lc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    struct { const wchar_t *t; int w; } cols[4] = { {c0,78},{c1,52},{c2,250},{c3,150} };
    for (int i = 0; i < 4; i++) {
        lc.iSubItem = i; lc.pszText = (LPWSTR)cols[i].t; lc.cx = cols[i].w;
        ListView_InsertColumn(h, i, &lc);
    }
    WNDPROC old = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)GridProc);
    SetPropW(h, L"oldproc", (HANDLE)old);
    return h;
}

static void create_ui(HWND hwnd)
{
    HINSTANCE hi = GetModuleHandle(NULL);
    const int CW = 1040, colw = 495, gap = 20, mx = 14;
    const int rx = mx + colw + gap;

    /* шапка */
    HWND brand = CreateWindowExW(0, L"STATIC",
        L"⌁ ASM WORKSHOP — ассемблер ⇄ деасемблер (NASM · Intel · DOSBox COM)",
        WS_CHILD | WS_VISIBLE | SS_LEFT, mx, 10, 720, 26, hwnd, NULL, hi, NULL);
    SendMessageW(brand, WM_SETFONT, (WPARAM)g_fBrand, TRUE);
    HWND sub = mk_static(hwnd, L"Чистый C · Win32 · таблицы байтов · Ctrl+Enter = собрать",
                         mx, 38, 720, 16);
    SendMessageW(sub, WM_SETFONT, (WPARAM)g_fUi, TRUE);

    g_hAboutBtn = mk_btn(hwnd, IDC_ABOUT_BTN, L"ℹ О программе", CW - 168, 12, 152, 30, FALSE);

    mk_static(hwnd, L"режим:", CW - 402, 50, 46, 16);
    g_hMode = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        CW - 352, 46, 336, 220, hwnd, (HMENU)(INT_PTR)IDC_MODE, hi, NULL);
    SendMessageW(g_hMode, CB_ADDSTRING, 0, (LPARAM)L"COM 16-bit (DOSBox, ORG 0x100)");
    SendMessageW(g_hMode, CB_ADDSTRING, 0, (LPARAM)L"16-bit raw");
    SendMessageW(g_hMode, CB_ADDSTRING, 0, (LPARAM)L"32-bit raw");
    SendMessageW(g_hMode, CB_ADDSTRING, 0, (LPARAM)L"64-bit raw");
    SendMessageW(g_hMode, CB_SETCURSEL, 0, 0);
    SendMessageW(g_hMode, WM_SETFONT, (WPARAM)g_fUi, TRUE);

    /* заголовки блоков */
    HWND t1 = mk_static(hwnd, L"АССЕМБЛИРОВАНИЕ   ASM → HEX", mx, 66, colw, 16);
    HWND t2 = mk_static(hwnd, L"ДЕАССЕМБЛИРОВАНИЕ   HEX → ASM", rx, 66, colw, 16);
    SendMessageW(t1, WM_SETFONT, (WPARAM)g_fCardTitle, TRUE);
    SendMessageW(t2, WM_SETFONT, (WPARAM)g_fCardTitle, TRUE);

    const int ih = 84, oh = 54, th = 190;
    int by = 88;

    /* левая колонка */
    g_hAsmIn = mk_edit(hwnd, IDC_ASM_IN, mx, by, colw, ih, 0);           by += ih + 8;
    g_hAsmBtn  = mk_btn(hwnd, IDC_ASM_BTN,  L"Собрать →",       mx, by, 110, 30, TRUE);
    g_hAsmCopy = mk_btn(hwnd, IDC_ASM_COPY, L"Копировать HEX",  mx+118, by, 130, 30, FALSE);
    g_hClearL  = mk_btn(hwnd, IDC_CLEAR_L,  L"Очистить", mx+colw-86, by, 86, 30, FALSE); by += 38;
    g_hAsmHexOut = mk_edit(hwnd, IDC_ASM_HEXOUT, mx, by, colw, oh, ES_READONLY);         by += oh + 8;
    g_hAsmGrid = mk_grid(hwnd, IDC_ASM_GRID, mx, by, colw, th,
                         L"Адрес", L"Байт", L"Пояснение", L"Режим");                      by += th + 4;
    g_hAsmMsg = mk_static(hwnd, L"", mx, by, colw, 34);

    /* правая колонка */
    int y2 = 88;
    g_hHexIn = mk_edit(hwnd, IDC_HEX_IN, rx, y2, colw, ih, 0);            y2 += ih + 8;
    g_hHexBtn  = mk_btn(hwnd, IDC_HEX_BTN,  L"← Разобрать",     rx, y2, 110, 30, TRUE);
    g_hHexCopy = mk_btn(hwnd, IDC_HEX_COPY, L"Копировать ASM",  rx+118, y2, 130, 30, FALSE);
    g_hClearR  = mk_btn(hwnd, IDC_CLEAR_R,  L"Очистить", rx+colw-86, y2, 86, 30, FALSE); y2 += 38;
    g_hHexOut = mk_edit(hwnd, IDC_HEX_OUT, rx, y2, colw, oh, ES_READONLY);               y2 += oh + 8;
    g_hHexGrid = mk_grid(hwnd, IDC_HEX_GRID, rx, y2, colw, th,
                         L"Смещение", L"№", L"Инструкция", L"Длина · режим");             y2 += th + 4;
    g_hHexMsg = mk_static(hwnd, L"", rx, y2, colw, 34);

    /* низ */
    g_hStatus = mk_static(hwnd,
        L"nasm + objdump · пример: mov ax, 0x4C00 / int 0x21 · байты: B8 01 00 00 00",
        mx, 668, 700, 16);
    g_hSwapBtn = mk_btn(hwnd, IDC_SWAP_BTN, L"результат → поле HEX-ввода",
                        CW - 250, 662, 234, 28, FALSE);

    /* шрифты */
    SendMessageW(g_hAsmIn, WM_SETFONT, (WPARAM)g_fMono, TRUE);
    SendMessageW(g_hHexIn, WM_SETFONT, (WPARAM)g_fMono, TRUE);
    SendMessageW(g_hAsmHexOut, WM_SETFONT, (WPARAM)g_fMono, TRUE);
    SendMessageW(g_hHexOut, WM_SETFONT, (WPARAM)g_fMono, TRUE);
    SendMessageW(g_hAsmGrid, WM_SETFONT, (WPARAM)g_fMono, TRUE);
    SendMessageW(g_hHexGrid, WM_SETFONT, (WPARAM)g_fMono, TRUE);
    SendMessageW(g_hAsmMsg, WM_SETFONT, (WPARAM)g_fUi, TRUE);
    SendMessageW(g_hHexMsg, WM_SETFONT, (WPARAM)g_fUi, TRUE);
    SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_fUi, TRUE);

    /* примеры + первый расчёт, чтобы таблицы были видны сразу */
    set_utf8(g_hAsmIn, "mov ax, 0x4C00\nint 0x21");
    set_utf8(g_hHexIn, "B8 01 00 00 00");
    do_assemble();
    do_disassemble();
}

/* owner-draw кнопок */
static void draw_button(LPDRAWITEMSTRUCT dis)
{
    BOOL prim = (BOOL)(intptr_t)GetPropW(dis->hwndItem, L"prim");
    COLORREF bg = prim ? RGB(0x0E,0xA5,0xE9) : RGB(0x23,0x28,0x37);
    COLORREF fg = prim ? RGB(0x08,0x10,0x18) : CLR_TEXT;
    if (dis->itemState & ODS_SELECTED)
        bg = prim ? RGB(0x38,0xBD,0xF8) : RGB(0x2D,0x34,0x46);
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dis->hDC, &dis->rcItem, br);
    DeleteObject(br);
    HPEN pen = CreatePen(PS_SOLID, 1, CLR_BORDER);
    HPEN op = (HPEN)SelectObject(dis->hDC, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, op); SelectObject(dis->hDC, ob); DeleteObject(pen);

    WCHAR txt[128] = L"";
    GetWindowTextW(dis->hwndItem, txt, 128);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    HFONT of = (HFONT)SelectObject(dis->hDC, g_fUi);
    DrawTextW(dis->hDC, txt, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dis->hDC, of);
}

/* ячейки/заголовки таблиц в тёмном стиле */
static LRESULT on_list_notify(LPNMHDR nh)
{
    if (nh->code == NM_CUSTOMDRAW) {
        LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)nh;
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            cd->clrText = CLR_TEXT;
            cd->clrTextBk = CLR_CARD;
            return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
            cd->clrText = CLR_TEXT;
            cd->clrTextBk = CLR_CARD;
            return CDRF_DODEFAULT;
        }
    }
    return CDRF_DODEFAULT;
}

/* ─────────── оконная процедура ─────────── */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hMain = hwnd;
        create_ui(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp; HWND child = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        if (child == g_hAsmMsg || child == g_hHexMsg)      SetTextColor(dc, CLR_NOTE);
        else if (child == g_hStatus)                        SetTextColor(dc, CLR_MUTED);
        else                                                SetTextColor(dc, CLR_TEXT);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, CLR_TEXT);
        SetBkColor(dc, CLR_EDIT);
        return (LRESULT)g_brEdit;
    }
    case WM_DRAWITEM:
        if (((LPDRAWITEMSTRUCT)lp)->CtlType == ODT_BUTTON) {
            draw_button((LPDRAWITEMSTRUCT)lp);
            return TRUE;
        }
        break;
    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->hwndFrom == g_hAsmGrid || nh->hwndFrom == g_hHexGrid)
            return on_list_notify(nh);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDC_ASM_BTN:  do_assemble(); return 0;
        case IDC_HEX_BTN:  do_disassemble(); return 0;
        case IDC_ASM_COPY: copy_text(g_last_hex); return 0;
        case IDC_HEX_COPY: copy_text(g_last_asm); return 0;
        case IDC_ABOUT_BTN: show_about(hwnd); return 0;
        case IDC_SWAP_BTN:
            if (g_last_hex[0]) set_utf8(g_hHexIn, g_last_hex);
            return 0;
        case IDC_CLEAR_L:
            SetWindowTextW(g_hAsmIn, L""); SetWindowTextW(g_hAsmHexOut, L"");
            SetWindowTextW(g_hAsmMsg, L""); table_fill(g_hAsmGrid, &g_td_asm, 0);
            g_last_hex[0] = 0; return 0;
        case IDC_CLEAR_R:
            SetWindowTextW(g_hHexIn, L""); SetWindowTextW(g_hHexOut, L"");
            SetWindowTextW(g_hHexMsg, L""); table_fill(g_hHexGrid, &g_td_hex, 0);
            g_last_asm[0] = 0; return 0;
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ─────────── main ─────────── */
int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd;

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    /* рабочий каталог движка (%TEMP%\asmworkshop) */
    char tmp[MAX_PATH];
    if (GetTempPathA(sizeof tmp, tmp)) { strcat(tmp, "asmworkshop"); _mkdir(tmp); }

    /* если nasm.exe лежит рядом с нашим exe — добавить его папку в PATH процесса,
     * чтобы popen("nasm ...") находил его без системной установки */
    char exepath[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exepath, MAX_PATH);
    char dir[MAX_PATH]; strcpy(dir, exepath);
    PathRemoveFileSpecA(dir);
    char probe[MAX_PATH]; strcpy(probe, dir); PathAppendA(probe, "nasm.exe");
    if (PathFileExistsA(probe)) {
        char oldpath[3600]; oldpath[0] = 0;
        GetEnvironmentVariableA("PATH", oldpath, sizeof oldpath);
        char newpath[4096];
        snprintf(newpath, sizeof newpath, "%s;%s", dir, oldpath);
        SetEnvironmentVariableA("PATH", newpath);
    }

    g_fUi = CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
    g_fMono = CreateFontW(-14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        FIXED_PITCH|FF_MODERN,L"Consolas");
    g_fBrand = CreateFontW(-19,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
    g_fCardTitle = CreateFontW(-12,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_SWISS,L"Segoe UI");

    g_brBg = CreateSolidBrush(CLR_BG);
    g_brCard = CreateSolidBrush(CLR_CARD);
    g_brEdit = CreateSolidBrush(CLR_EDIT);

    WNDCLASSW wc; ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_brBg;
    wc.lpszClassName = L"ASMWorkshopWnd";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE_W,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1056, 730,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    ShowWindow(hwnd, show ? show : SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        /* Ctrl+Enter = собрать/разобрать из активного поля */
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            HWND f = GetFocus();
            if (f == g_hAsmIn) { do_assemble(); continue; }
            if (f == g_hHexIn) { do_disassemble(); continue; }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
