// gobukti.go — bukti dunia nyata: binary GO STATIS (CGO_ENABLED=0, tanpa
// PT_INTERP) dijalankan lewat runner universal `fake-run`. Karena statis,
// LD_PRELOAD tidak berlaku, dan syscall dipanggil langsung (openat, stat,
// readlink, getdents64, …) — sehingga program ini otomatis jatuh ke jalur
// SUPERVISOR seccomp USER_NOTIF (`svsp`) yang me-rewrite path di kernel.
//
// Build (di Termux):
//   pkg install -y golang
//   CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -o gobukti gobukti.go
//
// Jalankan di dalam wadah palsu:
//   fake-run ./gobukti
//
// Catatan runtime: Go menginisialisasi netpoll (epoll_create1/epoll_ctl/
// eventfd2) saat membuka file pertama. Di arm64, `epoll_ctl` = 21 — nomor
// yang sama dengan rmdir pada tabel x86_64. Dulu filter `svsp` sempat
// men-*match* epoll_ctl sebagai "rmdir" (fallback `SYS_rmdir=21`) dan
// membalas EFAULT → "runtime: epollctl failed with 14" (lihat BRAINSTORM.md).
// Sudah diperbaiki: tidak ada syscall rmdir di arm64; rule di-guard
// `#ifdef SYS_rmdir`. Program ini adalah regresi-test untuk bug itu.
package main

import (
	"fmt"
	"os"
)

func check(label string, err error) {
	if err != nil {
		fmt.Printf("  %-28s -> GAGAL: %v\n", label, err)
		return
	}
	fmt.Printf("  %-28s -> OK\n", label)
}

func main() {
	fmt.Printf("[gobukti] Go statis asli — pid=%d cwd=%s\n", os.Getpid(), mustCwd())

	// 1) file di dalam wadah: /etc/alpine-release -> harus 3.24.2 (rewrite base+)
	b, err := os.ReadFile("/etc/alpine-release")
	fmt.Printf("  baca /etc/alpine-release   -> %q (err=%v)\n", trimNl(b), err)

	// 2) ISOLASI: file yang ADA di host tapi TIDAK boleh terlihat -> ENOENT
	_, err = os.Stat("/system/build.prop")
	check("stat /system/build.prop (ENOENT?)", err)
	if os.IsNotExist(err) {
		fmt.Println("  >>> ISOLASI TERBUKTI: /system/build.prop tidak terlihat (ENOENT)")
	}

	// 3) rantai tulis-baca-hapus di dalam wadah (/tmp ter-rewrite ke base/tmp)
	f := "/tmp/go-chain.txt"
	werr := os.WriteFile(f, []byte("go-chain-OK\n"), 0644)
	rb, rerr := os.ReadFile(f)
	derr := os.Remove(f)
	fmt.Printf("  chain tulis->baca->hapus      -> write=%v read=%q readerr=%v rm=%v\n",
		werr, trimNl(rb), rerr, derr)

	// 4) walk direktori wadah (openat ter-rewrite; getdents64 lewat fd itu)
	n, walkErr := fileCount("/etc")
	check(fmt.Sprintf("walk /etc (%d entri)", n), walkErr)

	// 5) /proc tetap passthrough host (exec native: exe = path di base)
	exe, err := os.Readlink("/proc/self/exe")
	fmt.Printf("  readlink /proc/self/exe       -> %s\n", exe)
}

func mustCwd() string {
	d, _ := os.Getwd()
	return d
}

func trimNl(b []byte) string {
	if len(b) == 0 {
		return ""
	}
	s := string(b)
	for len(s) > 0 && (s[len(s)-1] == '\n' || s[len(s)-1] == '\r') {
		s = s[:len(s)-1]
	}
	return s
}

func fileCount(dir string) (int, error) {
	entries, err := os.ReadDir(dir)
	return len(entries), err
}