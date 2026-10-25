// objc_darwin.go — привязка к Objective-C runtime macOS через CGo.
// Требует clang + macOS SDK (osxcross) для кросс-сборки; на самом Mac
// собирается командой: CC=o64-clang CGO_ENABLED=1 GOOS=darwin GOARCH=amd64 go build
//go:build darwin && cgo

package main

/*
#cgo LDFLAGS: -lobjc -framework Cocoa -framework Foundation
#include <objc/runtime.h>
#include <objc/message.h>
#include <stdlib.h>

static inline void* msg0(void* r, void* s){ return ((void*(*)(void*,void*))objc_msgSend)(r,s); }
static inline void* msg1(void* r, void* s, void* a1){ return ((void*(*)(void*,void*,void*))objc_msgSend)(r,s,a1); }
static inline void* msg2(void* r, void* s, void* a1, void* a2){ return ((void*(*)(void*,void*,void*))objc_msgSend)(r,s,a1,a2); }
static inline void* msg3(void* r, void* s, void* a1, void* a2, void* a3){ return ((void*(*)(void*,void*,void*))objc_msgSend)(r,s,a1,a2,a3); }
static inline void* msg4(void* r, void* s, void* a1, void* a2, void* a3, void* a4){ return ((void*(*)(void*,void*,void*))objc_msgSend)(r,s,a1,a2,a3,a4); }
static inline void* msgf1(void* r, void* s, double d1){ return ((void*(*)(void*,void*,double))objc_msgSend)(r,s,d1); }
static inline void* msgf4(void* r, void* s, double a, double b, double c, double d){ return ((void*(*)(void*,void*,double,double,double,double))objc_msgSend)(r,s,a,b,c,d); }
static inline void* clsN(const char* n){ return (void*)objc_getClass(n); }
static inline void* selN(const char* n){ return (void*)sel_registerName(n); }
*/
import "C"
import "unsafe"

type Objc = uintptr

func oSel(name string) Objc {
	p := C.CString(name)
	defer C.free(unsafe.Pointer(p))
	return Objc(C.selN(p))
}

func oCls(name string) Objc {
	p := C.CString(name)
	defer C.free(unsafe.Pointer(p))
	return Objc(C.clsN(p))
}

func m0(r Objc, s string) Objc {
	if r == 0 {
		return 0
	}
	return Objc(C.msg0(unsafe.Pointer(r), unsafe.Pointer(oSel(s))))
}
func m1(r Objc, s string, a1 Objc) Objc {
	if r == 0 {
		return 0
	}
	return Objc(C.msg1(unsafe.Pointer(r), unsafe.Pointer(oSel(s)), unsafe.Pointer(a1)))
}
func m2(r Objc, s string, a1, a2 Objc) Objc {
	if r == 0 {
		return 0
	}
	return Objc(C.msg2(unsafe.Pointer(r), unsafe.Pointer(oSel(s)), unsafe.Pointer(a1), unsafe.Pointer(a2)))
}
func m3(r Objc, s string, a1, a2, a3 Objc) Objc {
	if r == 0 {
		return 0
	}
	return Objc(C.msg3(unsafe.Pointer(r), unsafe.Pointer(oSel(s)), unsafe.Pointer(a1), unsafe.Pointer(a2), unsafe.Pointer(a3)))
}
func m4(r Objc, s string, a1, a2, a3, a4 Objc) Objc {
	if r == 0 {
		return 0
	}
	return Objc(C.msg4(unsafe.Pointer(r), unsafe.Pointer(oSel(s)), unsafe.Pointer(a1), unsafe.Pointer(a2), unsafe.Pointer(a3), unsafe.Pointer(a4)))
}
func mf1(r Objc, s string, d1 float64) Objc {
	if r == 0 {
		return 0
	}
	return Objc(C.msgf1(unsafe.Pointer(r), unsafe.Pointer(oSel(s)), C.double(d1)))
}
func mf4(r Objc, s string, a, b, c, d float64) Objc {
	if r == 0 {
		return 0
	}
	return Objc(C.msgf4(unsafe.Pointer(r), unsafe.Pointer(oSel(s)),
		C.double(a), C.double(b), C.double(c), C.double(d)))
}

// ─────────── помощники ───────────

func nsStr(s string) Objc {
	p := C.CString(s)
	defer C.free(unsafe.Pointer(p))
	return m1(oCls("NSString"), "stringWithUTF8String:", Objc(unsafe.Pointer(p)))
}

func allocInit(clsName string) Objc {
	o := m0(oCls(clsName), "alloc")
	return m0(o, "init")
}

type cgRect struct{ x, y, w, h float64 }

func setFrame(view Objc, x, y, w, h float64) {
	r := cgRect{x, y, w, h}
	m1(view, "setFrame:", Objc(unsafe.Pointer(&r)))
}

func addSubview(parent, child Objc) { m1(parent, "addSubview:", child) }
