/* SacredFire: Windows file-service probe for platform.h on a disposable directory: UTF-8 names
 * with spaces and non-ASCII characters, positioned reads and writes, directory listing with
 * types, stat times, rename over an existing file, truncate, directory descriptors, and the
 * contained tree removal that refuses paths outside its root. GPL-2.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "platform/platform.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); exit(1); } } while (0)
int main(void) {
    const char *base = getenv("BB_PROBE_DIR");
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s/sacredfire file probe \xc3\xa9\xe2\x9c\x93", base ? base : ".");
    platform_remove_tree(base ? base : ".", root);
    CHECK(platform_mkdir(root, 0755) == 0);
    CHECK(platform_mkdir(root, 0755) == -EEXIST);
    char dir[PATH_MAX], file[PATH_MAX], other[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/sub d\xc3\xadr", root);
    snprintf(file, sizeof(file), "%s/sub d\xc3\xadr/save \xe6\x97\xa5\xe6\x9c\xac.dat", root);
    snprintf(other, sizeof(other), "%s/replaced.dat", root);
    CHECK(platform_mkdir(dir, 0755) == 0);
    int fd = platform_open(file, 2 | 0x200 | 0x400, 0644); /* read-write: the positioned read below needs it */
    CHECK(fd >= 0);
    CHECK(platform_write(fd, "hello world", 11) == 11);
    CHECK(platform_pwrite(fd, "WORLD", 5, 6) == 5);
    CHECK(platform_fsync(fd) == 0);
    char buffer[32] = {0};
    CHECK(platform_pread(fd, buffer, 11, 0) == 11 && !strcmp(buffer, "hello WORLD"));
    CHECK(platform_lseek(fd, 0, 1) == 11); /* positioned I/O left the offset alone */
    CHECK(platform_ftruncate(fd, 5) == 0);
    PlatformStat st;
    CHECK(platform_fstat(fd, &st) == 0 && st.size == 5 && st.is_reg && st.mtim.tv_sec > 1600000000);
    CHECK(platform_close(fd) == 0);
    CHECK(platform_stat(file, &st) == 0 && st.size == 5);
    CHECK(platform_truncate(file, 3) == 0 && platform_stat(file, &st) == 0 && st.size == 3);
    /* Rename replaces an existing destination. */
    fd = platform_open(other, 1 | 0x200, 0644); CHECK(fd >= 0 && platform_write(fd, "old", 3) == 3 && platform_close(fd) == 0);
    CHECK(platform_rename(file, other) == 0);
    CHECK(platform_stat(file, &st) == -ENOENT && platform_stat(other, &st) == 0 && st.size == 3);
    /* Directory listing with types; a directory descriptor stats but does not read. */
    PlatformListing *l = platform_list_directory(root);
    CHECK(l && l->count == 2);
    int dirs = 0, files = 0;
    for (size_t i = 0; i < l->count; ++i) { if (l->types[i] == 4) ++dirs; if (l->types[i] == 8) ++files; }
    CHECK(dirs == 1 && files == 1);
    platform_free_listing(l);
    int dfd = platform_open(dir, 0, 0);
    CHECK(dfd >= 0 && platform_fstat(dfd, &st) == 0 && st.is_dir);
    CHECK(platform_read(dfd, buffer, 1) < 0);
    CHECK(platform_close(dfd) == 0);
    CHECK(platform_open(dir, 1, 0) == -EISDIR);
    CHECK(platform_open(other, 0x20000, 0) == -ENOTDIR);
    CHECK(platform_access(other, 4) == 0 && platform_access(file, 4) == -ENOENT);
    /* Containment: the tree removal refuses the root itself and anything outside it. */
    CHECK(platform_remove_tree(root, root) == -1);
    CHECK(platform_remove_tree(root, base ? base : ".") == -1);
    CHECK(platform_remove_tree(root, dir) == 0 && platform_stat(dir, &st) == -ENOENT);
    CHECK(platform_remove_tree(base ? base : ".", root) == 0 && platform_stat(root, &st) == -ENOENT);
    puts("file_probe PASS: UTF-8 names, positioned I/O, listing, rename-replace, truncate, directory descriptors, contained removal");
    return 0;
}
