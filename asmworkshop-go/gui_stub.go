// gui_stub.go — на не-Windows платформах GUI нет, работаем в CLI.
//go:build !windows

package main

func guiAvailable() bool { return false }

func guiMain() {}
