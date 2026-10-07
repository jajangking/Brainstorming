/*
 * pinterp.c — tulis ulang isi segmen PT_INTERP pada ELF64 (kasus IN-PLACE).
 *
 * Dipakai oleh bootstrap.sh: binary di dalam wadah (busybox, apk, ...) punya
 * PT_INTERP "/lib/ld-musl-aarch64.so.1" (26 byte). Saat dieksekusi langsung
 * oleh kernel HOST (jalur supervisor svsp), kernel mencari loader itu di
 * NAMESPACE host — padahal loader nyata ada sebagai file host absolut yang
 * jauh lebih panjang. pinterp menimpa string INTERP dengan path host itu.
 *
 * pinterp hanya menangani kasus path baru MUAT di p_filesz segmen lama
 * (tanpa mengubah ukuran file — idempoten, aman dijalankan ulang). Bila
 * tidak muat, pinterp keluar dengan kode 3 tanpa menyentuh file; pemanggil
 * (bootstrap.sh) meneruskan ke `patchelf --set-interpreter` — metode yang
 * terbukti untuk MEMPERBESAR segmen INTERP (membuat segmen baru di akhir
 * file, p_vaddr = p_offset + 0x10000, tanpa menggeser segmen lain).
 *
 * (Catatan: mencoba "append sendiri" gagal — kernel aarch64 menolak layout
 * dengan p_vaddr == p_offset pada segmen INTERP yang ditempel di akhir file;
 * patchelf menghasilkan layout yang diterima. Jangan kembali ke append manual.)
 *
 *   pinterp <file-elf64> <path-loader-baru>
 *
 * Kode keluar:
 *   0  -> berhasil ditulis (in-place)
 *   1  -> bukan ELF64 / tanpa PT_INTERP (skip diam-diam)
 *   2  -> I/O gagal (fatal)
 *   3  -> path tidak muat di segmen lama — butuh patchelf (file utuh)
 *
 * Tanpa dependensi selain libc; kompilasi: clang -O2 -o pinterp pinterp.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint8_t  u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

static u16 rd16(const u8 *p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8 *p) { u64 v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

#define ELFCLASS64 2
#define PT_INTERP  3
#define EI_CLASS   4

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "pemakaian: pinterp <elf64> <path-loader>\n"); return 2; }
    const char *path = argv[1], *loader = argv[2];
    size_t need = strlen(loader) + 1;

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        if (errno == ETXTBSY) {         /* file sedang dieksekusi kernel (mis. loader
                                           yang dipakai shell wadah yang masih hidup) —
                                           dilewati, bukan kesalahan fatal */
            fprintf(stderr, "%s: busy (sedang dieksekusi) — dilewati\n", path);
            return 4;
        }
        perror(path); return 2;
    }
    off_t fsz = lseek(fd, 0, SEEK_END);
    if (fsz < 64) { close(fd); return 1; }
    u8 *b = malloc((size_t)fsz);
    if (!b) { close(fd); return 2; }
    if (pread(fd, b, (size_t)fsz, 0) != fsz) { free(b); close(fd); return 2; }
    if (memcmp(b, "\x7f" "ELF", 4) != 0 || b[EI_CLASS] != ELFCLASS64) { free(b); close(fd); return 1; }

    u64 phoff = rd64(b + 32); u16 phentsz = rd16(b + 54); u16 phnum = rd16(b + 56);
    if (phentsz < 56 || phoff + (u64)phnum * phentsz > (u64)fsz) { free(b); close(fd); return 1; }
    const u8 *ph = b + phoff;

    int ii = -1;
    for (int i = 0; i < phnum; i++) if (rd32(ph + (u64)i * phentsz) == PT_INTERP) { ii = i; break; }
    if (ii < 0) { free(b); close(fd); return 1; }

    u8 *hip = b + phoff + (u64)ii * phentsz;
    u64 ioff = rd64(hip + 8), ifsz = rd64(hip + 32);

    if (need > ifsz) {
        /* tidak muat di tempat — biarkan patchelf yang memperbesar */
        free(b); close(fd); return 3;
    }
    if (ioff + need > (u64)fsz) { free(b); close(fd); return 2; }

    /* NUL bersih di seluruh segmen, lalu path */
    for (u64 x = 0; x < ifsz; x++) b[ioff + x] = 0;
    memcpy(b + ioff, loader, need);

    if (pwrite(fd, b, (size_t)fsz, 0) != fsz) { free(b); close(fd); return 2; }
    fsync(fd);
    free(b);
    close(fd);
    return 0;
}