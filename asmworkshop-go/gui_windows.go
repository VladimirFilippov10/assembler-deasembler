// gui_windows.go — ASM Workshop: бело-синий интерфейс (Go, Win32 API).
// Две карточки «Ассемблирование» и «Деасемблирование», таблицы разбора по полям,
// горячие клавиши Ctrl+Enter, раскрывающийся блок «О программе» (по умолчанию скрыт).
//go:build windows

package main

import (
	"fmt"
	"strings"
	"syscall"
	"unsafe"

	"github.com/lxn/win"
)

// ─────────── палитра ───────────

func hexColor(s string) win.COLORREF {
	s = strings.TrimPrefix(s, "#")
	var v uint32
	fmt.Sscanf(s, "%06x", &v)
	r, g, b := v>>16&0xFF, v>>8&0xFF, v&0xFF
	return win.COLORREF(b<<16 | g<<8 | r)
}

const (
	clWinBg    = "#F4F8FD"
	clCard     = "#FFFFFF"
	clCardEdge = "#CFE0F5"
	clInputBg  = "#F8FBFF"
	clOutBg    = "#EEF5FF"
	clHeadBg   = "#E0ECFB"
	clText     = "#0F2F55"
	clAccent   = "#1D4ED8"
	clAccent2  = "#2563EB"
	clBtn      = "#DBEAFE"
	clBtnHover = "#BFDBFE"
	clMuted    = "#5B7BA6"
	clOk       = "#15803D"
	clErr      = "#B91C1C"
	clWhite    = "#FFFFFF"
)

var (
	kernel32  = syscall.NewLazyDLL("kernel32.dll")
	user32    = syscall.NewLazyDLL("user32.dll")
	gdi32     = syscall.NewLazyDLL("gdi32.dll")
	getModule = kernel32.NewProc("GetModuleHandleW")
)

var (
	pCreateSolidBrush    = gdi32.NewProc("CreateSolidBrush")
	pCreatePen           = gdi32.NewProc("CreatePen")
	pCreateFontIndirectW = gdi32.NewProc("CreateFontIndirectW")
	pRoundRect           = gdi32.NewProc("RoundRect")
	pSelectObject        = gdi32.NewProc("SelectObject")
	pDeleteObject        = gdi32.NewProc("DeleteObject")
	pSetBkMode           = gdi32.NewProc("SetBkMode")
	pSetTextColor        = gdi32.NewProc("SetTextColor")
	pSetBkColor          = gdi32.NewProc("SetBkColor")
	pGetStockObject      = gdi32.NewProc("GetStockObject")
	pCreateCompatibleDC  = gdi32.NewProc("CreateCompatibleDC")
	pCreateCompatibleBmp = gdi32.NewProc("CreateCompatibleBitmap")
	pBitBlt              = gdi32.NewProc("BitBlt")
	pDeleteDC            = gdi32.NewProc("DeleteDC")
	pFillRect            = user32.NewProc("FillRect")
	pDrawTextW           = user32.NewProc("DrawTextW")
	pBeginPaint          = user32.NewProc("BeginPaint")
	pEndPaint            = user32.NewProc("EndPaint")
	pLoadCursorW         = user32.NewProc("LoadCursorW")
	pMoveToEx            = gdi32.NewProc("MoveToEx")
	pLineTo              = gdi32.NewProc("LineTo")
)

func createSolidBrush(c win.COLORREF) win.HBRUSH {
	h, _, _ := pCreateSolidBrush.Call(uintptr(c))
	return win.HBRUSH(h)
}

func createPen(style, w int32, c win.COLORREF) win.HPEN {
	h, _, _ := pCreatePen.Call(uintptr(style), uintptr(w), uintptr(c))
	return win.HPEN(h)
}

type logfont struct {
	LfHeight         int32
	LfWidth          int32
	LfEscapement     int32
	LfOrientation    int32
	LfWeight         uint32
	LfItalic         byte
	LfUnderline      byte
	LfStrikeOut      byte
	LfCharSet        byte
	LfOutPrecision   byte
	LfClipPrecision  byte
	LfQuality        byte
	LfPitchAndFamily byte
	LfFaceName       [32]uint16
}

func createFontW(name string, height int32) win.HFONT {
	lf := logfont{LfHeight: height, LfWeight: win.FW_NORMAL,
		LfCharSet: win.DEFAULT_CHARSET, LfOutPrecision: win.OUT_TT_PRECIS,
		LfClipPrecision: win.CLIP_DEFAULT_PRECIS, LfQuality: win.ANTIALIASED_QUALITY,
		LfPitchAndFamily: 0x01 | win.FF_SWISS}
	copy(lf.LfFaceName[:], utf16PtrBytes(name))
	h, _, _ := pCreateFontIndirectW.Call(uintptr(unsafe.Pointer(&lf)))
	return win.HFONT(h)
}

func utf16PtrBytes(s string) []uint16 {
	u, _ := syscall.UTF16FromString(s)
	return u
}

func fillRect(hdc win.HDC, r *win.RECT, hbr win.HBRUSH) {
	pFillRect.Call(uintptr(hdc), uintptr(unsafe.Pointer(r)), uintptr(hbr))
}

func roundRectHdc(hdc win.HDC, x1, y1, x2, y2, w, h int32) {
	pRoundRect.Call(uintptr(hdc), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), uintptr(w), uintptr(h))
}

func selectObj(hdc win.HDC, obj win.HGDIOBJ) win.HGDIOBJ {
	r, _, _ := pSelectObject.Call(uintptr(hdc), uintptr(obj))
	return win.HGDIOBJ(r)
}

func deleteObj(obj win.HGDIOBJ) { pDeleteObject.Call(uintptr(obj)) }

func setBkMode(hdc win.HDC, mode int32) int32 {
	r, _, _ := pSetBkMode.Call(uintptr(hdc), uintptr(mode))
	return int32(r)
}

func setTextColor(hdc win.HDC, c win.COLORREF) win.COLORREF {
	r, _, _ := pSetTextColor.Call(uintptr(hdc), uintptr(c))
	return win.COLORREF(r)
}

func setBkColor(hdc win.HDC, c win.COLORREF) win.COLORREF {
	r, _, _ := pSetBkColor.Call(uintptr(hdc), uintptr(c))
	return win.COLORREF(r)
}

func getStockNullBrush() win.HBRUSH {
	r, _, _ := pGetStockObject.Call(uintptr(win.NULL_BRUSH))
	return win.HBRUSH(r)
}

func drawTextW(hdc win.HDC, text string, rc *win.RECT, flags uint32) {
	p := utf16Ptr(text)
	pDrawTextW.Call(uintptr(hdc), uintptr(unsafe.Pointer(p)), ^uintptr(0),
		uintptr(unsafe.Pointer(rc)), uintptr(flags))
}

func beginPaint(hwnd win.HWND, ps *win.PAINTSTRUCT) win.HDC {
	r, _, _ := pBeginPaint.Call(uintptr(hwnd), uintptr(unsafe.Pointer(ps)))
	return win.HDC(r)
}

func endPaint(hwnd win.HWND, ps *win.PAINTSTRUCT) {
	pEndPaint.Call(uintptr(hwnd), uintptr(unsafe.Pointer(ps)))
}

func loadCursorApp() win.HCURSOR {
	r, _, _ := pLoadCursorW.Call(0, uintptr(32512))
	return win.HCURSOR(r)
}

func createCompatibleDC(hdc win.HDC) win.HDC {
	r, _, _ := pCreateCompatibleDC.Call(uintptr(hdc))
	return win.HDC(r)
}

func createCompatibleBitmap(hdc win.HDC, w, h int32) win.HBITMAP {
	r, _, _ := pCreateCompatibleBmp.Call(uintptr(hdc), uintptr(w), uintptr(h))
	return win.HBITMAP(r)
}

func bitBlt(dst win.HDC, x, y, w, h int32, src win.HDC, sx, sy int32, rop uint32) {
	pBitBlt.Call(uintptr(dst), uintptr(x), uintptr(y), uintptr(w), uintptr(h),
		uintptr(src), uintptr(sx), uintptr(sy), uintptr(rop))
}

func deleteDC(hdc win.HDC) { pDeleteDC.Call(uintptr(hdc)) }

func lineTo(hdc win.HDC, x, y int32) {
	pLineTo.Call(uintptr(hdc), uintptr(x), uintptr(y))
}

func moveTo(hdc win.HDC, x, y int32) {
	pMoveToEx.Call(uintptr(hdc), uintptr(x), uintptr(y), 0)
}

// ─────────── идентификаторы элементов ───────────

const (
	IDMode      uintptr = 100
	IDAsmIn     uintptr = 101
	IDAsmOut    uintptr = 102
	IDAsmNote   uintptr = 103
	IDHexIn     uintptr = 104
	IDHexOut    uintptr = 105
	IDHexNote   uintptr = 106
	IDLVLeft    uintptr = 107
	IDLVRight   uintptr = 108
	IDStatus    uintptr = 109
	BTNAssemble uintptr = 120
	BTNCopyHex  uintptr = 121
	BTNPasteAsm uintptr = 122
	BTNClearAsm uintptr = 123
	BTNDisasm   uintptr = 124
	BTNCopyAsm  uintptr = 125
	BTNPasteHex uintptr = 126
	BTNClearHex uintptr = 127
	BTNAbout    uintptr = 128
	BTNSwap     uintptr = 129
)

type ctl struct {
	hwnd win.HWND
	text string
	back win.COLORREF
	fg   win.COLORREF
}

type cardArea struct {
	x, y, cx, cy int32
	title        string
}

var (
	mainWnd  win.HWND
	fontUI   win.HFONT
	fontMono win.HFONT
	fontBig  win.HFONT

	modeBox   *ctl
	asmIn     *ctl
	asmOut    *ctl
	asmNote   *ctl
	hexIn     *ctl
	hexOut    *ctl
	hexNote   *ctl
	lvLeft    *ctl
	lvRight   *ctl
	statusLbl *ctl

	statics []*ctl
	buttons []*ctl
	cards   []*cardArea

	// Раскрывающийся блок «О программе».
	// По умолчанию закрыт; открывается/закрывается кнопкой «О программе».
	aboutVisible bool
	aboutCard    *cardArea // область-подложка блока (рисуется в WM_PAINT)
)

func guiAvailable() bool { return true }

func utf16Ptr(s string) *uint16 {
	p, _ := syscall.UTF16PtrFromString(s)
	return p
}

func setText(c *ctl, s string) {
	win.SendMessage(c.hwnd, win.WM_SETTEXT, 0, uintptr(unsafe.Pointer(utf16Ptr(s))))
}

func getText(c *ctl) string {
	n := int(win.SendMessage(c.hwnd, win.WM_GETTEXTLENGTH, 0, 0))
	buf := make([]uint16, n+1)
	win.SendMessage(c.hwnd, win.WM_GETTEXT, uintptr(n+1), uintptr(unsafe.Pointer(&buf[0])))
	return syscall.UTF16ToString(buf[:n])
}

func mkStatic(parent win.HWND, x, y, w, h int32, text string, style uint32, fg win.COLORREF) *ctl {
	hwnd := win.CreateWindowEx(0, utf16Ptr("STATIC"), utf16Ptr(text),
		uint32(win.WS_CHILD|win.WS_VISIBLE)|style, x, y, w, h, parent, 0, inst(), nil)
	return &ctl{hwnd: hwnd, text: text, fg: fg}
}

func mkEdit(parent win.HWND, x, y, w, h int32, id uintptr, multiline bool, ro bool, back win.COLORREF) *ctl {
	st := uint32(win.WS_CHILD | win.WS_VISIBLE | win.WS_BORDER | win.WS_TABSTOP)
	if multiline {
		st |= uint32(win.ES_MULTILINE | win.ES_AUTOVSCROLL | win.WS_VSCROLL)
	}
	if ro {
		st |= uint32(win.ES_READONLY)
	}
	hwnd := win.CreateWindowEx(uint32(win.WS_EX_CLIENTEDGE), utf16Ptr("EDIT"), utf16Ptr(""),
		st, x, y, w, h, parent, win.HMENU(id), inst(), nil)
	return &ctl{hwnd: hwnd, back: back}
}

func mkButton(parent win.HWND, x, y, w, h int32, text string, id uintptr, primary bool) *ctl {
	hwnd := win.CreateWindowEx(0, utf16Ptr("BUTTON"), utf16Ptr(text),
		uint32(win.WS_CHILD|win.WS_VISIBLE|win.WS_TABSTOP|win.BS_OWNERDRAW),
		x, y, w, h, parent, win.HMENU(id), inst(), nil)
	c := &ctl{hwnd: hwnd, text: text}
	btnMap[id] = c
	if primary {
		c.back = hexColor(clAccent2)
		c.fg = hexColor(clWhite)
	} else {
		c.back = hexColor(clBtn)
		c.fg = hexColor("#1E3A8A")
	}
	return c
}

func mkCombo(parent win.HWND, x, y, w int32, id uintptr) *ctl {
	hwnd := win.CreateWindowEx(0, utf16Ptr("COMBOBOX"), utf16Ptr(""),
		uint32(win.WS_CHILD|win.WS_VISIBLE|win.WS_TABSTOP|win.CBS_DROPDOWNLIST|win.CBS_HASSTRINGS|win.WS_VSCROLL),
		x, y, w, 240, parent, win.HMENU(id), inst(), nil)
	return &ctl{hwnd: hwnd}
}

func mkListview(parent win.HWND, x, y, w, h int32, id uintptr) *ctl {
	hwnd := win.CreateWindowEx(uint32(win.WS_EX_CLIENTEDGE), utf16Ptr("SysListView32"), utf16Ptr(""),
		uint32(win.WS_CHILD|win.WS_VISIBLE|win.LVS_REPORT|win.LVS_SHOWSELALWAYS),
		x, y, w, h, parent, win.HMENU(id), inst(), nil)
	lvb := &win.LVCOLUMN{Mask: win.LVCF_FMT | win.LVCF_WIDTH | win.LVCF_TEXT,
		Fmt: win.LVCFMT_LEFT}
	heads := []string{"№", "Префикс", "Opcode", "Операнды", "Immediate", "Размер, байт"}
	widths := []int32{40, 80, 80, 220, 110, 90}
	for i, hd := range heads {
		lvb.Cx = widths[i]
		lvb.PszText = utf16Ptr(hd)
		lvb.ISubItem = int32(i)
		win.SendMessage(hwnd, win.LVM_INSERTCOLUMN, uintptr(i), uintptr(unsafe.Pointer(lvb)))
	}
	return &ctl{hwnd: hwnd}
}

func inst() win.HINSTANCE {
	h, _, _ := getModule.Call(0)
	return win.HINSTANCE(h)
}

// ─────────── layout ───────────

const margin = 12

func relayout() {
	var rc win.RECT
	win.GetClientRect(mainWnd, &rc)
	W, H := rc.Right, rc.Bottom

	colW := (W - margin*3) / 2
	headH := int32(64)

	modeX := W - colW/2 - 20
	win.MoveWindow(modeBox.hwnd, modeX, 18, colW/2, 200, true)

	y := headH
	cardH := H - y - 44
	left := cards[0]
	right := cards[1]

	// Если блок «О программе» открыт — показываем его третьей колонкой,
	// иначе две колонки на всю ширину.
	aboutW := int32(0)
	if aboutVisible {
		aboutW = 340
		if W < 1100 {
			aboutW = 300
		}
	}
	avail := W - margin*3 - aboutW
	colW = avail / 2

	setCard(left, margin, y, colW, cardH)
	setCard(right, margin*2+colW, y, colW, cardH)
	if aboutVisible && aboutCard != nil {
		setCard(aboutCard, margin*3+colW*2, y, W-margin*3-colW*2, cardH)
	} else if aboutCard != nil {
		// спрятать область — нулевые размеры
		setCard(aboutCard, 0, 0, 0, 0)
	}
	layoutCard(left, true)
	layoutCard(right, false)
	win.MoveWindow(statusLbl.hwnd, margin, H-32, W-2*margin-160, 24, true)
}

func setCard(c *cardArea, x, y, w, h int32) {
	c.x, c.y, c.cx, c.cy = x, y, w, h
}

func layoutCard(c *cardArea, left bool) {
	in := asmIn
	out := asmOut
	note := asmNote
	lv := lvLeft
	bAsm := btnByID(BTNAssemble)
	bCopy := btnByID(BTNCopyHex)
	bPaste := btnByID(BTNPasteAsm)
	bClear := btnByID(BTNClearAsm)
	if !left {
		in = hexIn
		out = hexOut
		note = hexNote
		lv = lvRight
		bAsm = btnByID(BTNDisasm)
		bCopy = btnByID(BTNCopyAsm)
		bPaste = btnByID(BTNPasteHex)
		bClear = btnByID(BTNClearHex)
	}
	x, y, w := c.x+14, c.y+40, c.cx-28
	btnRow := int32(30)
	noteH := int32(58)
	gap := int32(8)
	lvH := (c.cy - 54 - noteH - btnRow*2 - gap*3) / 2
	inH := c.cy - 54 - noteH - btnRow - gap*2 - lvH
	if inH < 60 {
		inH = 60
	}
	move(in.hwnd, x, y, w, inH)
	y += inH + gap
	move(bAsm.hwnd, x, y, 130, btnRow)
	move(bCopy.hwnd, x+138, y, 130, btnRow)
	move(bPaste.hwnd, x+276, y, 90, btnRow)
	move(bClear.hwnd, x+w-80, y, 80, btnRow)
	y += btnRow + gap
	move(note.hwnd, x, y, w, noteH)
	y += noteH + gap
	move(out.hwnd, x, y, w, lvH)
	y += lvH + gap
	move(lv.hwnd, x, y, w, c.cy-(y-c.y)-20)
}

func move(hwnd win.HWND, x, y, w, h int32) { win.MoveWindow(hwnd, x, y, w, h, true) }

var btnMap = map[uintptr]*ctl{}

func btnByID(id uintptr) *ctl { return btnMap[id] }

// ─────────── запуск GUI ───────────

func guiMain() {
	cls := win.WNDCLASSEX{
		CbSize:        uint32(unsafe.Sizeof(win.WNDCLASSEX{})),
		Style:         win.CS_HREDRAW | win.CS_VREDRAW,
		LpfnWndProc:   syscall.NewCallback(wndProc),
		HInstance:     inst(),
		HbrBackground: createSolidBrush(hexColor(clWinBg)),
		LpszClassName: utf16Ptr("ASMWorkshopGo"),
		HCursor:       loadCursorApp(),
	}
	win.RegisterClassEx(&cls)

	fontUI = mkFont("Segoe UI", 10)
	fontMono = mkFont("Consolas", 11)
	fontBig = mkFont("Segoe UI", 14)

	w, h := int32(1120), int32(720)
	mainWnd = win.CreateWindowEx(0, utf16Ptr("ASMWorkshopGo"),
		utf16Ptr("ASM Workshop — ассемблер ⇄ деасемблер"),
		win.WS_OVERLAPPEDWINDOW,
		win.CW_USEDEFAULT, win.CW_USEDEFAULT, w, h,
		0, 0, inst(), nil)

	createChildren()
	relayout()
	win.ShowWindow(mainWnd, win.SW_SHOW)
	win.UpdateWindow(mainWnd)

	doAssemble()
	doDisassemble()

	var msg win.MSG
	for win.GetMessage(&msg, 0, 0, 0) != 0 {
		win.TranslateMessage(&msg)
		win.DispatchMessage(&msg)
	}
}

func mkFont(name string, size int) win.HFONT {
	return createFontW(name, -int32(size)*96/72)
}

func createChildren() {
	// шапка
	brand := mkStatic(mainWnd, margin, 10, 600, 24, "⌁ ASM WORKSHOP", 0, hexColor(clAccent))
	brand.setFont(fontBig)
	sub := mkStatic(mainWnd, margin, 38, 600, 18, "ассемблер ⇄ деасемблер · NASM · Intel · DOSBox COM · Go", 0, hexColor(clMuted))
	statics = append(statics, brand, sub)

	modeBox = mkCombo(mainWnd, 0, 0, 300, IDMode)
	for _, s := range []string{"COM 16-bit (DOSBox, ORG 0x100)", "16-bit raw", "32-bit raw", "64-bit raw"} {
		win.SendMessage(modeBox.hwnd, win.CB_ADDSTRING, 0, uintptr(unsafe.Pointer(utf16Ptr(s))))
	}
	win.SendMessage(modeBox.hwnd, win.CB_SETCURSEL, 0, 0)

	// карточки-подложки (только две; «О программе» — отдельная область)
	cards = append(cards,
		&cardArea{title: "АССЕМБЛИРОВАНИЕ   ASM → HEX"},
		&cardArea{title: "ДЕАССЕМБЛИРОВАНИЕ   HEX → ASM"})

	// левая колонка
	asmIn = mkEdit(mainWnd, 0, 0, 0, 0, IDAsmIn, true, false, hexColor(clInputBg))
	buttons = append(buttons,
		mkButton(mainWnd, 0, 0, 0, 0, "Собрать →", BTNAssemble, true),
		mkButton(mainWnd, 0, 0, 0, 0, "Копировать HEX", BTNCopyHex, false),
		mkButton(mainWnd, 0, 0, 0, 0, "Вставить", BTNPasteAsm, false),
		mkButton(mainWnd, 0, 0, 0, 0, "Очистить", BTNClearAsm, false))
	asmOut = mkEdit(mainWnd, 0, 0, 0, 0, IDAsmOut, true, true, hexColor(clOutBg))
	asmNote = mkStatic(mainWnd, 0, 0, 0, 0, "", win.SS_LEFT|win.SS_NOPREFIX, hexColor(clMuted))
	lvLeft = mkListview(mainWnd, 0, 0, 0, 0, IDLVLeft)

	// правая колонка
	hexIn = mkEdit(mainWnd, 0, 0, 0, 0, IDHexIn, true, false, hexColor(clInputBg))
	buttons = append(buttons,
		mkButton(mainWnd, 0, 0, 0, 0, "← Разобрать", BTNDisasm, true),
		mkButton(mainWnd, 0, 0, 0, 0, "Копировать ASM", BTNCopyAsm, false),
		mkButton(mainWnd, 0, 0, 0, 0, "Вставить", BTNPasteHex, false),
		mkButton(mainWnd, 0, 0, 0, 0, "Очистить", BTNClearHex, false))
	hexOut = mkEdit(mainWnd, 0, 0, 0, 0, IDHexOut, true, true, hexColor(clOutBg))
	hexNote = mkStatic(mainWnd, 0, 0, 0, 0, "", win.SS_LEFT|win.SS_NOPREFIX, hexColor(clMuted))
	lvRight = mkListview(mainWnd, 0, 0, 0, 0, IDLVRight)

	// кнопки шапки/низа
	buttons = append(buttons,
		mkButton(mainWnd, 0, 0, 0, 0, "О программе", BTNAbout, false),
		mkButton(mainWnd, 0, 0, 0, 0, "результат → поле HEX-ввода", BTNSwap, false))

	// блок «О программе» — область подложки, содержимое рисуется в WM_PAINT.
	// По умолчанию закрыт (aboutVisible = false), размеры нулевые.
	aboutCard = &cardArea{title: ""}
	setCard(aboutCard, 0, 0, 0, 0)

	statusLbl = mkStatic(mainWnd, 0, 0, 0, 0,
		"nasm + objdump · введите команду (mov ax, 1) или байты (B8 01 00) · Ctrl+Enter = собрать",
		0, hexColor(clMuted))

	setFontsAll()

	setText(asmIn, "mov ax, 0x4C00\nint 0x21")
	setText(hexIn, "B8 01 00 00 00")
}

func (c *ctl) setFont(f win.HFONT) {
	win.SendMessage(c.hwnd, win.WM_SETFONT, uintptr(f), 1)
}

func setFontsAll() {
	all := []*ctl{modeBox, asmIn, asmOut, asmNote, hexIn, hexOut, hexNote, lvLeft, lvRight, statusLbl}
	all = append(all, buttons...)
	all = append(all, statics...)
	for _, c := range all {
		if c == nil || c.hwnd == 0 {
			continue
		}
		c.setFont(fontUI)
	}
	for _, c := range []*ctl{asmIn, asmOut, hexIn, hexOut} {
		c.setFont(fontMono)
	}
}

// ─────────── оконная процедура ───────────

func wndProc(hWnd win.HWND, msg uint32, wParam, lParam uintptr) uintptr {
	switch msg {
	case win.WM_CTLCOLORSTATIC:
		hdc := win.HDC(wParam)
		tgt := win.HWND(lParam)
		setBkMode(hdc, win.TRANSPARENT)
		var bg win.COLORREF
		var fg win.COLORREF
		switch tgt {
		case asmNote.hwnd:
			fg = noteColor(asmNote)
			bg = hexColor(clCard)
		case hexNote.hwnd:
			fg = noteColor(hexNote)
			bg = hexColor(clCard)
		default:
			fg = hexColor(clMuted)
			if isCardStatic(tgt) {
				bg = hexColor(clCard)
			} else {
				bg = hexColor(clWinBg)
			}
		}
		setTextColor(hdc, fg)
		setBkColor(hdc, bg)
		return uintptr(getStockNullBrush())

	case win.WM_CTLCOLOREDIT:
		hdc := win.HDC(wParam)
		tgt := win.HWND(lParam)
		bg := hexColor(clInputBg)
		fg := hexColor(clText)
		switch tgt {
		case asmOut.hwnd, hexOut.hwnd:
			bg = hexColor(clOutBg)
			fg = hexColor("#1E40AF")
		}
		setTextColor(hdc, fg)
		setBkColor(hdc, bg)
		return uintptr(createSolidBrush(bg))

	case win.WM_DRAWITEM:
		di := (*win.DRAWITEMSTRUCT)(unsafe.Pointer(lParam))
		drawButton(di)
		return 1

	case win.WM_SIZE:
		relayout()
		invalidateCards()
		win.InvalidateRect(mainWnd, nil, true)
		return 0

	case win.WM_PAINT:
		var ps win.PAINTSTRUCT
		hdc := beginPaint(hWnd, &ps)
		paintCards(hdc)
		if aboutVisible && aboutCard != nil && aboutCard.cx > 0 {
			paintAbout(hdc, aboutCard)
		}
		endPaint(hWnd, &ps)
		return 0

	case win.WM_COMMAND:
		id := uintptr(wParam & 0xFFFF)
		notify := uintptr((wParam >> 16) & 0xFFFF)
		switch id {
		case BTNAssemble:
			doAssemble()
		case BTNDisasm:
			doDisassemble()
		case BTNCopyHex:
			copyText(strings.ReplaceAll(getText(asmOut), "\r\n", " "))
		case BTNCopyAsm:
			copyText(getText(hexOut))
		case BTNPasteAsm:
			if t, ok := pasteText(); ok {
				setText(asmIn, t)
			}
		case BTNPasteHex:
			if t, ok := pasteText(); ok {
				setText(hexIn, t)
			}
		case BTNClearAsm:
			setText(asmIn, "")
			setText(asmOut, "")
			setNote(asmNote, "", false)
			clearLV(lvLeft)
		case BTNClearHex:
			setText(hexIn, "")
			setText(hexOut, "")
			setNote(hexNote, "", false)
			clearLV(lvRight)
		case BTNAbout:
			toggleAbout()
		case BTNSwap:
			t := strings.ReplaceAll(getText(asmOut), "\r\n", " ")
			if strings.TrimSpace(t) != "" {
				setText(hexIn, t)
				doDisassemble()
			}
		case IDMode:
			if notify == win.CBN_SELCHANGE {
				doAssemble()
				doDisassemble()
			}
		}
		return 0

	case win.WM_KEYDOWN:
		ks, _, _ := getKeyState.Call(uintptr(win.VK_CONTROL))
		ctrl := ks&0x8000 != 0
		if ctrl && (wParam == win.VK_RETURN || wParam == win.VK_ADD || wParam == 0x6D) {
			focus := win.GetFocus()
			if focus == asmIn.hwnd {
				doAssemble()
			} else if focus == hexIn.hwnd {
				doDisassemble()
			}
			return 0
		}

	case win.WM_DESTROY:
		win.PostQuitMessage(0)
		return 0
	}
	return win.DefWindowProc(hWnd, msg, wParam, lParam)
}

// ─────────── «О программе»: открыть/закрыть ───────────

func toggleAbout() {
	aboutVisible = !aboutVisible
	relayout()
	win.InvalidateRect(mainWnd, nil, true)
}

// ─────────── отрисовка карточек и кнопок ───────────

func paintCards(hdc win.HDC) {
	for _, c := range cards {
		if c.cx <= 0 {
			continue
		}
		rect := win.RECT{c.x, c.y, c.x + c.cx, c.y + c.cy}
		brush := createSolidBrush(hexColor(clCard))
		fillRect(hdc, &rect, brush)
		deleteObj(win.HGDIOBJ(brush))
		pen := createPen(win.PS_SOLID, 1, hexColor(clCardEdge))
		old := selectObj(hdc, win.HGDIOBJ(pen))
		roundRectHdc(hdc, c.x, c.y, c.x+c.cx, c.y+c.cy, 18, 18)
		selectObj(hdc, old)
		deleteObj(win.HGDIOBJ(pen))
		// заголовок
		setBkMode(hdc, win.TRANSPARENT)
		setTextColor(hdc, hexColor(clAccent2))
		oldf := selectObj(hdc, win.HGDIOBJ(fontUI))
		tr := win.RECT{c.x + 14, c.y + 10, c.x + c.cx - 8, c.y + 34}
		drawTextW(hdc, c.title, &tr, uint32(win.DT_LEFT|win.DT_SINGLELINE|win.DT_VCENTER))
		selectObj(hdc, oldf)
	}
}

// paintAbout — блок «О программе» по образцу WPF:
// две колонки (О ПРОГРАММЕ / КРАТКИЙ ГАЙД), жёлтый разделитель,
// нижняя плашка с разработчиком.
func paintAbout(hdc win.HDC, c *cardArea) {
	// фон и рамка карточки
	rect := win.RECT{c.x, c.y, c.x + c.cx, c.y + c.cy}
	brush := createSolidBrush(hexColor("#FFFDF5"))
	fillRect(hdc, &rect, brush)
	deleteObj(win.HGDIOBJ(brush))
	pen := createPen(win.PS_SOLID, 1, hexColor("#FFD700"))
	old := selectObj(hdc, win.HGDIOBJ(pen))
	roundRectHdc(hdc, c.x, c.y, c.x+c.cx, c.y+c.cy, 18, 18)
	selectObj(hdc, old)
	deleteObj(win.HGDIOBJ(pen))

	setBkMode(hdc, win.TRANSPARENT)
	oldf := selectObj(hdc, win.HGDIOBJ(fontUI))
	defer selectObj(hdc, oldf)

	pad := int32(16)
	colW := (c.cx - pad*3) / 2
	col1X := c.x + pad
	col2X := c.x + pad*2 + colW
	topY := c.y + pad

	// ── заголовки колонок ──
	setTextColor(hdc, hexColor("#DAA520"))
	r1 := win.RECT{col1X, topY, col1X + colW, topY + 22}
	drawTextW(hdc, "📌 О ПРОГРАММЕ", &r1, uint32(win.DT_LEFT|win.DT_SINGLELINE|win.DT_VCENTER))
	r2 := win.RECT{col2X, topY, col2X + colW, topY + 22}
	drawTextW(hdc, "📖 КРАТКИЙ ГАЙД", &r2, uint32(win.DT_LEFT|win.DT_SINGLELINE|win.DT_VCENTER))

	// ── разделитель ──
	divX := c.x + pad + colW + pad/2
	divPen := createPen(win.PS_SOLID, 1, hexColor("#FFD700"))
	oldp := selectObj(hdc, win.HGDIOBJ(divPen))
	moveTo(hdc, divX, topY)
	lineTo(hdc, divX, c.y+c.cy-pad)
	selectObj(hdc, oldp)
	deleteObj(win.HGDIOBJ(divPen))

	// ── левая колонка: назначение ──
	setTextColor(hdc, hexColor(clText))
	bodyTop := topY + 28
	body := "CRC-32 (Cyclic Redundancy Check) — алгоритм вычисления " +
		"контрольной суммы, используемый для проверки целостности данных.\r\n\r\n" +
		"Данная программа позволяет:\r\n" +
		"• Вычислять CRC-32 для любого текста\r\n" +
		"• Наблюдать пошаговую работу регистра сдвига\r\n" +
		"• Искать исходные данные по известному CRC-ключу"
	rb := win.RECT{col1X, bodyTop, col1X + colW, c.y + c.cy - 60}
	drawTextW(hdc, body, &rb, uint32(win.DT_LEFT|win.DT_WORDBREAK|win.DT_NOPREFIX))

	// ── правая колонка: гайд ──
	guide := "🔐 Шифрование (вкладка \"ШИФРОВАНИЕ\"):\r\n" +
		"1. Введите текст в поле «Входные данные»\r\n" +
		"2. Нажмите «ВЫЧИСЛИТЬ CRC-32» для быстрого расчета\r\n" +
		"3. Нажмите «ПОШАГОВЫЙ РЕЖИМ» для детального просмотра\r\n" +
		"4. Используйте «КОПИРОВАТЬ» чтобы сохранить результат\r\n\r\n" +
		"🔓 Дешифрование (вкладка \"ДЕШИФРОВАНИЕ\"):\r\n" +
		"1. Введите CRC-32 ключ (например, 0x8BD69E57)\r\n" +
		"2. Нажмите «РАСШИФРОВАТЬ»\r\n" +
		"3. Программа найдет данные в базе (если есть)\r\n\r\n" +
		"💡 Результаты сохраняются в crc_database.txt"
	rg := win.RECT{col2X, bodyTop, col2X + colW, c.y + c.cy - 60}
	drawTextW(hdc, guide, &rg, uint32(win.DT_LEFT|win.DT_WORDBREAK|win.DT_NOPREFIX))

	// ── нижняя плашка с разработчиком ──
	devRect := win.RECT{c.x + pad, c.y + c.cy - 44, c.x + c.cx - pad, c.y + c.cy - 12}
	devBrush := createSolidBrush(hexColor("#FFF0D0"))
	fillRect(hdc, &devRect, devBrush)
	deleteObj(win.HGDIOBJ(devBrush))
	devPen := createPen(win.PS_SOLID, 1, hexColor("#FFD700"))
	olddp := selectObj(hdc, win.HGDIOBJ(devPen))
	roundRectHdc(hdc, devRect.Left, devRect.Top, devRect.Right, devRect.Bottom, 8, 8)
	selectObj(hdc, olddp)
	deleteObj(win.HGDIOBJ(devPen))

	setTextColor(hdc, hexColor("#8B6914"))
	drawTextW(hdc, "👨‍💻 Разработчик: Филиппов Владимир Евгеньевич | Группа: 25ПИнж(б)-1",
		&devRect, uint32(win.DT_CENTER|win.DT_SINGLELINE|win.DT_VCENTER|win.DT_NOPREFIX))
}

func invalidateCards() {
	for _, c := range cards {
		if c.cx > 0 {
			win.InvalidateRect(mainWnd, &win.RECT{c.x, c.y, c.x + c.cx, c.y + c.cy}, true)
		}
	}
}

func drawButton(di *win.DRAWITEMSTRUCT) {
	c := btnByID(uintptr(di.CtlID))
	back := c.back
	fg := c.fg
	if di.ItemState&win.ODS_SELECTED != 0 {
		back = hexColor(clBtnHover)
		if c.text == "Собрать →" || c.text == "← Разобрать" {
			back = hexColor(clAccent)
		}
	}
	rect := di.RcItem
	w := rect.Right - rect.Left
	h := rect.Bottom - rect.Top
	mem := createCompatibleDC(di.HDC)
	bmp := createCompatibleBitmap(di.HDC, w, h)
	old := selectObj(mem, win.HGDIOBJ(bmp))
	brush := createSolidBrush(back)
	fillRect(mem, &win.RECT{0, 0, w, h}, brush)
	deleteObj(win.HGDIOBJ(brush))
	pen := createPen(win.PS_SOLID, 1, hexColor(clCardEdge))
	oldp := selectObj(mem, win.HGDIOBJ(pen))
	roundRectHdc(mem, 0, 0, w-1, h-1, 12, 12)
	selectObj(mem, oldp)
	deleteObj(win.HGDIOBJ(pen))
	// текст
	oldf := selectObj(mem, win.HGDIOBJ(fontUI))
	setBkMode(mem, win.TRANSPARENT)
	setTextColor(mem, fg)
	tr := win.RECT{6, 0, w - 6, h}
	drawTextW(mem, c.text, &tr, uint32(win.DT_CENTER|win.DT_SINGLELINE|win.DT_VCENTER))
	bitBlt(di.HDC, rect.Left, rect.Top, w, h, mem, 0, 0, win.SRCCOPY)
	selectObj(mem, oldf)
	selectObj(mem, old)
	deleteDC(mem)
	deleteObj(win.HGDIOBJ(bmp))
}

func isCardStatic(hwnd win.HWND) bool {
	return hwnd == asmNote.hwnd || hwnd == hexNote.hwnd
}

func noteColor(c *ctl) win.COLORREF {
	if strings.HasPrefix(c.text, "✕") {
		return hexColor(clErr)
	}
	return hexColor(clOk)
}

// ─────────── действия ───────────

func currentMode() Mode {
	i := int(win.SendMessage(modeBox.hwnd, win.CB_GETCURSEL, 0, 0))
	return Mode(i)
}

func setNote(c *ctl, msg string, isErr bool) {
	prefix := "✓ готово"
	if isErr {
		prefix = "✕ ошибка"
	}
	full := prefix
	if msg != "" {
		full += "\n" + firstLineMulti(msg)
	}
	c.text = full
	setText(c, strings.ReplaceAll(full, "\n", "\r\n"))
	win.InvalidateRect(mainWnd, nil, false)
}

func firstLineMulti(msg string) string {
	lines := strings.Split(msg, "\n")
	out := lines[0]
	if len(lines) > 1 {
		out += " · " + strings.TrimSpace(lines[len(lines)-1])
	}
	return out
}

func doAssemble() {
	src := strings.ReplaceAll(getText(asmIn), "\r\n", "\n")
	hex, res := Assemble(src, currentMode())
	if res.IsErr {
		setText(asmOut, "")
		setNote(asmNote, res.Text, true)
		clearLV(lvLeft)
		return
	}
	setText(asmOut, hex)
	setNote(asmNote, res.Text, false)
	table, summary, r2 := Breakdown(src, currentMode(), false)
	if !r2.IsErr {
		fillLV(lvLeft, table)
		setStatus(summary)
	} else {
		clearLV(lvLeft)
	}
}

func doDisassemble() {
	hx := strings.ReplaceAll(getText(hexIn), "\r\n", "\n")
	a, res := Disassemble(hx, currentMode())
	if res.IsErr {
		setText(hexOut, "")
		setNote(hexNote, res.Text, true)
		clearLV(lvRight)
		return
	}
	setText(hexOut, a)
	setNote(hexNote, res.Text, false)
	table, summary, r2 := Breakdown(hx, currentMode(), true)
	if !r2.IsErr {
		fillLV(lvRight, table)
		setStatus(summary)
	} else {
		clearLV(lvRight)
	}
}

func setStatus(s string) {
	statusLbl.text = s
	setText(statusLbl, s)
}

func clearLV(c *ctl) {
	win.SendMessage(c.hwnd, win.LVM_DELETEALLITEMS, 0, 0)
}

func fillLV(c *ctl, table string) {
	clearLV(c)
	for _, ln := range strings.Split(table, "\n") {
		if strings.Count(ln, "|") < 5 || strings.Contains(ln, "----+") {
			continue
		}
		if strings.Contains(ln, "Opcode") && strings.Contains(ln, "Operands") {
			continue
		}
		cols := strings.Split(ln, "|")
		for i := range cols {
			cols[i] = strings.TrimSpace(cols[i])
		}
		if len(cols) < 6 {
			continue
		}
		lvi := win.LVITEM{Mask: win.LVIF_TEXT, IItem: -1,
			ISubItem: 0, PszText: utf16Ptr(cols[0])}
		lvi.IItem = int32(win.SendMessage(c.hwnd, win.LVM_INSERTITEM, 0, uintptr(unsafe.Pointer(&lvi))))
		for k := 1; k < 6; k++ {
			sub := win.LVITEM{Mask: win.LVIF_TEXT, IItem: lvi.IItem, ISubItem: int32(k),
				PszText: utf16Ptr(cols[k])}
			win.SendMessage(c.hwnd, win.LVM_SETITEM, 0, uintptr(unsafe.Pointer(&sub)))
		}
	}
}

// ─────────── буфер обмена ───────────

func copyText(s string) {
	data := append(syscall.StringToUTF16(s), 0)
	const CF_UNICODETEXT = 13
	hmem, _, _ := kernel32GlobalAlloc.Call(2 /*GMEM_MOVEABLE*/, uintptr(len(data)*2))
	if hmem == 0 {
		return
	}
	p, _, _ := kernel32GlobalLock.Call(hmem)
	copy(*(*[]uint16)(unsafe.Pointer(&struct {
		p pointer
		n int
	}{pointer(p), len(data)})), data)
	kernel32GlobalUnlock.Call(hmem)
	if win.OpenClipboard(mainWnd) {
		win.EmptyClipboard()
		win.SetClipboardData(CF_UNICODETEXT, win.HANDLE(hmem))
		win.CloseClipboard()
	}
}

type pointer = unsafe.Pointer

var (
	kernel32GlobalAlloc  = kernel32.NewProc("GlobalAlloc")
	kernel32GlobalLock   = kernel32.NewProc("GlobalLock")
	kernel32GlobalUnlock = kernel32.NewProc("GlobalUnlock")
	isClipFmt            = user32.NewProc("IsClipboardFormatAvailable")
	openClip             = user32.NewProc("OpenClipboard")
	closeClip            = user32.NewProc("CloseClipboard")
	getClipData          = user32.NewProc("GetClipboardData")
	globalSize           = kernel32.NewProc("GlobalSize")
	getKeyState          = user32.NewProc("GetKeyState")
)

func pasteText() (string, bool) {
	const CF_UNICODETEXT = 13
	r, _, _ := isClipFmt.Call(CF_UNICODETEXT)
	if r == 0 {
		return "", false
	}
	openClip.Call(uintptr(mainWnd))
	defer closeClip.Call()
	h, _, _ := getClipData.Call(CF_UNICODETEXT)
	if h == 0 {
		return "", false
	}
	p, _, _ := kernel32GlobalLock.Call(h)
	if p == 0 {
		return "", false
	}
	size, _, _ := globalSize.Call(h)
	buf := unsafe.Slice((*uint16)(pointer(p)), size/2)
	kernel32GlobalUnlock.Call(h)
	s := syscall.UTF16ToString(buf)
	return strings.ReplaceAll(s, "\r\n", "\n"), true
}