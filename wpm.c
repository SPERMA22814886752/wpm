// wpm - Wilix Package Manager

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

/* ---------- конфигурация (root only) ---------- */
#define BASE_URL    "https://raw.githubusercontent.com/SPERMA22814886752/wpm-repo/main"
#define PACKAGES_FILE "Packages"
#define INSTALLED_DB  "installed.list"
#define POOL_DIR    "pool/main"

static const char *install_prefix(void) {
    return "/usr";
}

static const char *lib_dir(void) {
    return "/usr/lib";
}

static const char *cache_dir(void) {
    return "/var/cache/wpm";
}

/* ---------- утилиты ---------- */
static int mkdir_p(const char *dir) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755);
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static size_t file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return st.st_size;
    return 0;
}

/* ---------- загрузка ---------- */
static int download_with_progress(const char *url, const char *output, size_t expected_size) {
    printf("\n");
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("wget", "wget", "--no-check-certificate", "-O", output, url, NULL);
        _exit(127);
    }
    if (pid < 0) {
        fprintf(stderr, "Error: fork failed\n");
        return -1;
    }
    if (expected_size > 0) {
        printf("[");
        fflush(stdout);
        int last_percent = 0;
        while (1) {
            int status;
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                printf("\r[====================] 100%%\n");
                fflush(stdout);
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
                if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
                    fprintf(stderr, "Error: wget not found. Install wget first.\n");
                    return -1;
                }
                fprintf(stderr, "Download failed (exit code: %d)\n",
                        WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                return -1;
            }
            size_t current = file_size(output);
            if (current > 0 && expected_size > 0) {
                int percent = (int)(current * 100 / expected_size);
                if (percent > last_percent && percent <= 100) {
                    last_percent = percent;
                    printf("\r[");
                    int bars = percent / 5;
                    for (int i = 0; i < 20; i++) {
                        if (i < bars) printf("=");
                        else if (i == bars) printf(">");
                        else printf(" ");
                    }
                    printf("] %d%%", percent);
                    fflush(stdout);
                }
            }
            usleep(50000);
        }
    } else {
        int spin = 0;
        const char spinner[] = "/-\\|";
        int status;
        while (waitpid(pid, &status, WNOHANG) == 0) {
            printf("\r\033[KDownloading %c", spinner[spin++ % 4]);
            fflush(stdout);
            usleep(100000);
        }
        printf("\r\033[K");
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("Download complete\n");
            return 0;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            fprintf(stderr, "Error: wget not found\n");
            return -1;
        } else {
            fprintf(stderr, "Download failed\n");
            return -1;
        }
    }
}

static int download_packages_list(const char *url, const char *output) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("wget", "wget", "--no-check-certificate", "-O", output, url, NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int spin = 0;
    const char spinner[] = "/|\\-";
    int status;
    fprintf(stderr, "Search the file ");
    fflush(stderr);
    while (waitpid(pid, &status, WNOHANG) == 0) {
        fprintf(stderr, "\r\033[KSearch the file %c", spinner[spin++ % 4]);
        fflush(stderr);
        usleep(100000);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "\r\033[KSearch the file OK\n");
        return 0;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        fprintf(stderr, "\r\033[KError: wget not found\n");
        return -1;
    } else {
        fprintf(stderr, "\r\033[KSearch the file ERROR\n");
        return -1;
    }
}

/* ---------- структура пакета ---------- */
struct Package {
    char name[128];
    char version[64];
    char type[16];
    char filename[256];
    size_t size;
    char md5sum[64];
    char destdir[128];
    char libc_type[8];
    char so_libc_path[256];
    char a_libc_path[256];
    bool is_archive;
};

static struct Package packages[100];
static int pkg_count = 0;

/* ---------- парсинг Packages ---------- */
static void parse_packages_file(const char *filepath) {
    pkg_count = 0;
    FILE *f = fopen(filepath, "r");
    if (!f) return;

    char line[1024];
    struct Package p;
    memset(&p, 0, sizeof(p));

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) {
            if (p.name[0] && pkg_count < 100) {
                const char *fname = p.filename;
                if (fname[0]) {
                    size_t flen = strlen(fname);
                    if ((flen > 7 && !strcmp(fname + flen - 7, ".tar.gz")) ||
                        (flen > 4 && !strcmp(fname + flen - 4, ".tar")))
                        p.is_archive = true;
                }
                packages[pkg_count++] = p;
                memset(&p, 0, sizeof(p));
            }
            continue;
        }
        if (line[0] == ' ' || line[0] == '#') continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *key = line;
        char *value = colon + 1;
        while (*value == ' ') value++;

        if (!strcmp(key, "Package")) strncpy(p.name, value, sizeof(p.name)-1);
        else if (!strcmp(key, "Version")) strncpy(p.version, value, sizeof(p.version)-1);
        else if (!strcmp(key, "Type")) strncpy(p.type, value, sizeof(p.type)-1);
        else if (!strcmp(key, "Filename")) strncpy(p.filename, value, sizeof(p.filename)-1);
        else if (!strcmp(key, "Size")) p.size = strtoul(value, NULL, 10);
        else if (!strcmp(key, "MD5sum") || !strcmp(key, "MD5")) strncpy(p.md5sum, value, sizeof(p.md5sum)-1);
        else if (!strcmp(key, "DestDir")) strncpy(p.destdir, value, sizeof(p.destdir)-1);
        else if (!strcmp(key, "libc")) strncpy(p.libc_type, value, sizeof(p.libc_type)-1);
        else if (!strcmp(key, "so libc")) strncpy(p.so_libc_path, value, sizeof(p.so_libc_path)-1);
        else if (!strcmp(key, "a libc")) strncpy(p.a_libc_path, value, sizeof(p.a_libc_path)-1);
    }
    if (p.name[0] && pkg_count < 100) {
        const char *fname = p.filename;
        if (fname[0]) {
            size_t flen = strlen(fname);
            if ((flen > 7 && !strcmp(fname + flen - 7, ".tar.gz")) ||
                (flen > 4 && !strcmp(fname + flen - 4, ".tar")))
                p.is_archive = true;
        }
        packages[pkg_count++] = p;
    }
    fclose(f);
}

static struct Package *find_package(const char *name) {
    for (int i = 0; i < pkg_count; i++)
        if (!strcmp(packages[i].name, name)) return &packages[i];
    return NULL;
}

/* ---------- проверка установленных библиотек ---------- */
static int lib_is_installed(const char *libname) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", lib_dir(), libname);
    return file_exists(path);
}

/* ---------- распаковка архива ---------- */
static int extract_archive(const char *archive_path, const char *dest_dir) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C \"%s\" 2>/dev/null", archive_path, dest_dir);
    return system(cmd) == 0 ? 0 : -1;
}

/* ---------- проверка ldd ---------- */
static void check_ldd(const char *binary_path) {
    const char *ext = strrchr(binary_path, '.');
    if (ext) {
        if (!strcmp(ext, ".a") || !strcmp(ext, ".la") ||
            !strcmp(ext, ".h") || !strcmp(ext, ".hpp") ||
            !strcmp(ext, ".pc") || !strcmp(ext, ".txt") ||
            !strcmp(ext, ".md") || !strcmp(ext, ".cmake"))
            return;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ldd \"%s\" 2>/dev/null", binary_path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char line[512];
    bool has_not_found = false;
    printf("ldd %s:\n", binary_path);
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        if (l > 0 && line[l-1] == '\n') line[l-1] = '\0';
        printf("  %s\n", line);
        if (strstr(line, "not found")) has_not_found = true;
    }
    pclose(fp);
    if (has_not_found)
        fprintf(stderr, "[ERROR 109] Error with install lib! Missing libraries (not found) in binary %s. Report to creator.\n", binary_path);
}

static void check_dir_ldd(const char *dir) {
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) check_dir_ldd(path);
        else if (S_ISREG(st.st_mode)) check_ldd(path);
    }
    closedir(dp);
}

/* ---------- локальная база установленных пакетов ---------- */
static void record_installed(const struct Package *pkg) {
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s", cache_dir(), INSTALLED_DB);
    mkdir_p(cache_dir());
    FILE *f = fopen(db_path, "a");
    if (!f) {
        perror("fopen installed.db");
        return;
    }
    fprintf(f, "%s|%s|%s|%s\n", pkg->name, pkg->version, pkg->type, pkg->destdir[0] ? pkg->destdir : "bin");
    fclose(f);
    chmod(db_path, 0644);
}

static int cmd_list_installed(void) {
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s", cache_dir(), INSTALLED_DB);
    FILE *f = fopen(db_path, "r");
    if (!f) {
        printf("No packages installed.\n");
        return 0;
    }
    printf("Installed packages:\n");
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;
        char *name = strtok(line, "|");
        char *ver  = strtok(NULL, "|");
        char *type = strtok(NULL, "|");
        char *dest = strtok(NULL, "|");
        printf("  %-20s %-10s %-6s %s\n", name ? name : "-",
               ver ? ver : "-", type ? type : "-", dest ? dest : "bin");
    }
    fclose(f);
    return 0;
}

/* ---------- команды ---------- */
static int cmd_list_packages(void) {
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%s", cache_dir(), PACKAGES_FILE);
    if (!file_exists(cache_path) || file_size(cache_path) < 10) {
        mkdir_p(cache_dir());
        char url[512];
        snprintf(url, sizeof(url), "%s/%s", BASE_URL, PACKAGES_FILE);
        if (download_packages_list(url, cache_path) != 0) {
            fprintf(stderr, "Failed to download package list\n");
            return 1;
        }
    }
    printf("\n");
    FILE *f = fopen(cache_path, "r");
    if (!f) { perror("fopen"); return 1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
    fclose(f);
    return 0;
}

static int cmd_add(const char *name) {
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%s", cache_dir(), PACKAGES_FILE);
    if (!file_exists(cache_path) || file_size(cache_path) < 10) {
        mkdir_p(cache_dir());
        char url[512];
        snprintf(url, sizeof(url), "%s/%s", BASE_URL, PACKAGES_FILE);
        if (download_packages_list(url, cache_path) != 0) {
            fprintf(stderr, "Failed to download package list\n");
            return 1;
        }
    }
    parse_packages_file(cache_path);
    struct Package *pkg = find_package(name);
    if (!pkg) {
        fprintf(stderr, "Package '%s' not found.\n\n", name);
        if (pkg_count > 0) {
            fprintf(stderr, "Available packages:\n");
            for (int i = 0; i < pkg_count; i++)
                fprintf(stderr, "  - %-20s %-10s %-6s %zu bytes\n",
                        packages[i].name, packages[i].version,
                        packages[i].type, packages[i].size);
        }
        return 1;
    }
    printf("\n%s (%s)\n", pkg->name, pkg->type[0] ? pkg->type : "bin");

    // либс зависимости
    if (pkg->libc_type[0] && strcmp(pkg->libc_type, "no") != 0) {
        bool need_so = (!strcmp(pkg->libc_type, "so") || !strcmp(pkg->libc_type, "both"));
        bool need_a  = (!strcmp(pkg->libc_type, "a")  || !strcmp(pkg->libc_type, "both"));
        if (need_so && pkg->so_libc_path[0]) {
            const char *libname = strrchr(pkg->so_libc_path, '/') + 1;
            if (!lib_is_installed(libname)) {
                printf("Installing required library: %s\n", libname);
                char lib_url[512];
                snprintf(lib_url, sizeof(lib_url), "%s/%s", BASE_URL, pkg->so_libc_path);
                char tmp_lib[512];
                snprintf(tmp_lib, sizeof(tmp_lib), "%s/%s.download", cache_dir(), libname);
                mkdir_p(cache_dir());
                if (download_with_progress(lib_url, tmp_lib, 0) != 0) {
                    fprintf(stderr, "Failed to download library %s\n", libname);
                    return 1;
                }
                char dest_lib[512];
                snprintf(dest_lib, sizeof(dest_lib), "%s/%s", lib_dir(), libname);
                mkdir_p(lib_dir());
                if (rename(tmp_lib, dest_lib) != 0) {
                    perror("rename library");
                    unlink(tmp_lib);
                    return 1;
                }
                chmod(dest_lib, 0644);
                printf("Library %s installed\n", libname);
            }
        }
        if (need_a && pkg->a_libc_path[0]) {
            const char *libname = strrchr(pkg->a_libc_path, '/') + 1;
            if (!lib_is_installed(libname)) {
                printf("Installing required static library: %s\n", libname);
                char lib_url[512];
                snprintf(lib_url, sizeof(lib_url), "%s/%s", BASE_URL, pkg->a_libc_path);
                char tmp_lib[512];
                snprintf(tmp_lib, sizeof(tmp_lib), "%s/%s.download", cache_dir(), libname);
                if (download_with_progress(lib_url, tmp_lib, 0) != 0) {
                    fprintf(stderr, "Failed to download static library %s\n", libname);
                    return 1;
                }
                char dest_lib[512];
                snprintf(dest_lib, sizeof(dest_lib), "%s/%s", lib_dir(), libname);
                if (rename(tmp_lib, dest_lib) != 0) {
                    perror("rename static library");
                    unlink(tmp_lib);
                    return 1;
                }
                chmod(dest_lib, 0644);
                printf("Static library %s installed\n", libname);
            }
        }
    }

    // установка чего то
    char url[512];
    if (pkg->filename[0])
        snprintf(url, sizeof(url), "%s/%s", BASE_URL, pkg->filename);
    else {
        const char *pool = (!strcmp(pkg->type, "lib") || !strcmp(pkg->type, "so") ||
                            !strcmp(pkg->type, "a") || !strcmp(pkg->type, "la"))
                           ? "pool/main/libs" : "pool/main";
        snprintf(url, sizeof(url), "%s/%s/%s", BASE_URL, pool, pkg->name);
    }
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.download", cache_dir(), pkg->name);
    mkdir_p(cache_dir());
    printf("Update cache\n");
    if (download_with_progress(url, tmp_path, pkg->size) != 0) {
        fprintf(stderr, "Download failed\n");
        unlink(tmp_path);
        return 1;
    }

    // определяем директорию установки
    char dest_dir[512];
    if (pkg->destdir[0])
        snprintf(dest_dir, sizeof(dest_dir), "%s/%s", install_prefix(), pkg->destdir);
    else if (!strcmp(pkg->type, "lib") || !strcmp(pkg->type, "so") ||
             !strcmp(pkg->type, "a")  || !strcmp(pkg->type, "la"))
        snprintf(dest_dir, sizeof(dest_dir), "%s", lib_dir());
    else
        snprintf(dest_dir, sizeof(dest_dir), "%s/bin", install_prefix());
    mkdir_p(dest_dir);

    if (pkg->is_archive) {
        printf("Extracting archive to %s\n", dest_dir);
        if (extract_archive(tmp_path, dest_dir) != 0) {
            unlink(tmp_path);
            return 1;
        }
        unlink(tmp_path);
        char chmod_cmd[1024];
        snprintf(chmod_cmd, sizeof(chmod_cmd),
                 "find \"%s\" -type f ! -name \"*.so\" ! -name \"*.a\" ! -name \"*.la\" -exec chmod +x {} \\; 2>/dev/null", dest_dir);
        system(chmod_cmd);
    } else {
        char dest_path[512];
        snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, pkg->name);
        if (rename(tmp_path, dest_path) != 0) {
            perror("rename");
            unlink(tmp_path);
            return 1;
        }
        if (!strcmp(pkg->type, "bin") || !strcmp(pkg->type, "script"))
            chmod(dest_path, 0755);
        else if (!strcmp(pkg->type, "lib") || !strcmp(pkg->type, "so") ||
                 !strcmp(pkg->type, "a")  || !strcmp(pkg->type, "la"))
            chmod(dest_path, 0644);
    }

    printf("\nInstall success! -> %s\n", dest_dir);
    record_installed(pkg);

    if (!strcmp(pkg->type, "bin") || !strcmp(pkg->type, "dir")) {
        printf("\nChecking library dependencies...\n");
        check_dir_ldd(dest_dir);
    }
    return 0;
}

/* ---------- удаление пакета ---------- */
static int cmd_remove(const char *name) {
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s", cache_dir(), INSTALLED_DB);

    FILE *f = fopen(db_path, "r");
    if (!f) {
        fprintf(stderr, "Package '%s' is not installed.\n", name);
        return 1;
    }

    char line[512];
    char found_version[64] = {0};
    char found_type[16]    = {0};
    char found_dest[128]   = {0};

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;

        char tmp[512];
        strncpy(tmp, line, sizeof(tmp)-1);
        char *n    = strtok(tmp, "|");
        char *ver  = strtok(NULL, "|");
        char *type = strtok(NULL, "|");
        char *dest = strtok(NULL, "|");

        if (n && !strcmp(n, name)) {
            if (ver)  strncpy(found_version, ver,  sizeof(found_version)-1);
            if (type) strncpy(found_type,    type, sizeof(found_type)-1);
            if (dest) strncpy(found_dest,    dest, sizeof(found_dest)-1);
            break;
        }
    }
    fclose(f);

    if (!found_version[0]) {
        fprintf(stderr, "Package '%s' is not installed.\n", name);
        return 1;
    }

    // определяем путь к файлу/директории
    char target[512];
    if (found_dest[0])
        snprintf(target, sizeof(target), "%s/%s/%s", install_prefix(), found_dest, name);
    else if (!strcmp(found_type, "lib") || !strcmp(found_type, "so") ||
             !strcmp(found_type, "a")   || !strcmp(found_type, "la"))
        snprintf(target, sizeof(target), "%s/%s", lib_dir(), name);
    else
        snprintf(target, sizeof(target), "%s/bin/%s", install_prefix(), name);

    printf("Removing %s (%s)...\n", name, found_version);

    // удаляем файл или директорию
    struct stat st;
    if (stat(target, &st) != 0) {
        fprintf(stderr, "Warning: '%s' not found on disk, removing from DB only.\n", target);
    } else if (S_ISDIR(st.st_mode)) {
        char cmd[768];
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", target);
        if (system(cmd) != 0) {
            fprintf(stderr, "Failed to remove directory %s\n", target);
            return 1;
        }
    } else {
        if (unlink(target) != 0) {
            perror("unlink");
            return 1;
        }
    }

    // перезаписываем installed.list без удалённой записи
    char db_tmp[512];
    snprintf(db_tmp, sizeof(db_tmp), "%s/%s.tmp", cache_dir(), INSTALLED_DB);

    FILE *in  = fopen(db_path, "r");
    FILE *out = fopen(db_tmp, "w");
    if (!in || !out) {
        perror("fopen db");
        if (in)  fclose(in);
        if (out) fclose(out);
        return 1;
    }

    char buf[512];
    while (fgets(buf, sizeof(buf), in)) {
        char tmp2[512];
        strncpy(tmp2, buf, sizeof(tmp2)-1);
        char *n = strtok(tmp2, "|");
        if (n && !strcmp(n, name)) continue;
        fputs(buf, out);
    }
    fclose(in);
    fclose(out);

    if (rename(db_tmp, db_path) != 0) {
        perror("rename db");
        unlink(db_tmp);
        return 1;
    }
    chmod(db_path, 0644);

    printf("Removed %s -> %s\n", name, target);
    return 0;
}

static int cmd_delete_cache(void) {
    const char *dir = cache_dir();
    printf("Delete cache\n");
    DIR *dp = opendir(dir);
    if (!dp) {
        printf("[===========>]\nSuccess! (cache empty)\n");
        return 0;
    }
    printf("[");
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if (unlink(path) == 0) { count++; printf("="); fflush(stdout); }
    }
    closedir(dp);
    rmdir(dir);
    printf("]\nSuccess!\n");
    return 0;
}

static int cmd_update(void) {
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%s", cache_dir(), PACKAGES_FILE);
    mkdir_p(cache_dir());
    char url[512];
    snprintf(url, sizeof(url), "%s/%s", BASE_URL, PACKAGES_FILE);
    printf("Update cache\n");
    if (download_packages_list(url, cache_path) != 0) {
        fprintf(stderr, "Failed to update cache\n");
        return 1;
    }
    parse_packages_file(cache_path);
    printf("Cache updated (%d packages)\n", pkg_count);
    return 0;
}

static int cmd_search(const char *query) {
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%s", cache_dir(), PACKAGES_FILE);
    if (!file_exists(cache_path) || file_size(cache_path) < 10) cmd_update();
    parse_packages_file(cache_path);
    int found = 0;
    printf("\nSearch results for '%s':\n\n", query);
    for (int i = 0; i < pkg_count; i++) {
        if (strstr(packages[i].name, query)) {
            printf("  %-20s %-10s [%-4s] %8zu bytes\n",
                   packages[i].name, packages[i].version,
                   packages[i].type, packages[i].size);
            found++;
        }
    }
    if (!found) printf("  No packages found\n");
    else printf("\nFound %d package(s)\n", found);
    return 0;
}

static void usage(const char *prog) {
    printf("WPM - Wilix Package Manager (root required except list -i)\n\n");
    printf("Usage:\n");
    printf("  %s add <pkg>            Install package (root)\n", prog);
    printf("  %s remove <pkg>         Remove installed package (root)\n", prog);
    printf("  %s list                 Show raw Packages file (root)\n", prog);
    printf("  %s list -i              Show installed packages (no root)\n", prog);
    printf("  %s search <query>       Search for packages (root)\n", prog);
    printf("  %s update               Force cache update (root)\n", prog);
    printf("  %s --delete-cache       Clear local cache (root)\n", prog);
    printf("\nBinaries -> /usr/bin, Libraries -> /usr/lib\n");
}

int main(int argc, char **argv) {
    int need_root = 1;
    if (argc >= 2) {
        if (!strcmp(argv[1], "list") && argc >= 3 && !strcmp(argv[2], "-i"))
            need_root = 0;
        else if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))
            need_root = 0;
    }

    if (need_root && geteuid() != 0) {
        fprintf(stderr, "[ERROR 10] not start of doas/sudo\n");
        return 10;
    }

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    if (!strcmp(argv[1], "add") && argc == 3)
        return cmd_add(argv[2]);
    else if (!strcmp(argv[1], "remove") && argc == 3)
        return cmd_remove(argv[2]);
    else if (!strcmp(argv[1], "list")) {
        if (argc >= 3 && !strcmp(argv[2], "-i"))
            return cmd_list_installed();
        else
            return cmd_list_packages();
    }
    else if (!strcmp(argv[1], "search") && argc == 3)
        return cmd_search(argv[2]);
    else if (!strcmp(argv[1], "update"))
        return cmd_update();
    else if (!strcmp(argv[1], "--delete-cache"))
        return cmd_delete_cache();
    else if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        usage(argv[0]);
        return 0;
    }
    else {
        fprintf(stderr, "Unknown command: %s\n\n", argv[1]);
        usage(argv[0]);
        return 1;
    }
}
