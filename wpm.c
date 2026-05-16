/*
 * WPM - Wilix Package Manager
*/


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

/* ---------- конфигурация ---------- */
#define BASE_URL    "https://raw.githubusercontent.com/SPERMA22814886752/wpm-repo/main"
#define BASE_PATH   ""
#define PACKAGES_FILE "Packages"
#define POOL_DIR    "pool/main"

static const char *install_prefix(void) {
    static char buf[256];
    if (geteuid() == 0) return "/usr/local";
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, sizeof(buf), "%s/.local", home);
    return buf;
}

static const char *cache_dir(void) {
    static char buf[256];
    if (geteuid() == 0) snprintf(buf, sizeof(buf), "/var/cache/wpm");
    else {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(buf, sizeof(buf), "%s/.cache/wpm", home);
    }
    return buf;
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
    
    // запускаем wget
    pid_t pid = fork();
    if (pid == 0) {
        // перенаправляем вывод wget в /dev/null
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("wget", "wget", "--no-check-certificate", "-q", "-O", output, url, NULL);
        _exit(127);
    }
    
    if (pid < 0) {
        fprintf(stderr, "Error: fork failed\n");
        return -1;
    }
    
    // свой прогресс-бар
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
                
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    return 0;
                } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
                    fprintf(stderr, "Error: wget not found. Install wget first.\n");
                    return -1;
                } else {
                    fprintf(stderr, "Download failed (exit code: %d)\n", 
                            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                    return -1;
                }
            }
            
            // проверяем текущий размер файла
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
            
            usleep(50000); // 50ms
        }
    } else {
        // размер неизвестен - показываем спиннер
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

/* ---------- загрузка Packages ---------- */
static int download_packages_list(const char *url, const char *output) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("wget", "wget", "--no-check-certificate", "-q", "-O", output, url, NULL);
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
    char filename[256];
    size_t size;
    char md5[64];
};

static struct Package packages[100];
static int pkg_count = 0;

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
                packages[pkg_count++] = p;
                memset(&p, 0, sizeof(p));
            }
            continue;
        }
        
        if (line[0] == ' ') continue;
        
        char *colon = strchr(line, ':');
        if (!colon) continue;
        
        *colon = '\0';
        char *key = line;
        char *value = colon + 1;
        while (*value == ' ') value++;
        
        if (!strcmp(key, "Package"))
            strncpy(p.name, value, sizeof(p.name) - 1);
        else if (!strcmp(key, "Version"))
            strncpy(p.version, value, sizeof(p.version) - 1);
        else if (!strcmp(key, "Filename"))
            strncpy(p.filename, value, sizeof(p.filename) - 1);
        else if (!strcmp(key, "Size"))
            p.size = strtoul(value, NULL, 10);
        else if (!strncmp(key, "MD5", 3))
            strncpy(p.md5, value, sizeof(p.md5) - 1);
    }
    
    if (p.name[0] && pkg_count < 100) {
        packages[pkg_count++] = p;
    }
    
    fclose(f);
}

static struct Package *find_package(const char *name) {
    for (int i = 0; i < pkg_count; i++) {
        if (!strcmp(packages[i].name, name))
            return &packages[i];
    }
    return NULL;
}

/* ---------- команды ---------- */
static int cmd_list_packages(void) {
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%s", cache_dir(), PACKAGES_FILE);
    
    if (!file_exists(cache_path) || file_size(cache_path) < 10) {
        mkdir_p(cache_dir());
        char url[512];
        snprintf(url, sizeof(url), "%s%s/%s", BASE_URL, BASE_PATH, PACKAGES_FILE);
        
        if (download_packages_list(url, cache_path) != 0) {
            fprintf(stderr, "Failed to download package list\n");
            return 1;
        }
    }
    
    printf("\n");
    FILE *f = fopen(cache_path, "r");
    if (!f) {
        perror("fopen");
        return 1;
    }
    
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, n, stdout);
    fclose(f);
    
    return 0;
}

static int cmd_add(const char *name) {
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%s", cache_dir(), PACKAGES_FILE);
    
    // обновляем кэш если нужно
    if (!file_exists(cache_path) || file_size(cache_path) < 10) {
        mkdir_p(cache_dir());
        char url[512];
        snprintf(url, sizeof(url), "%s%s/%s", BASE_URL, BASE_PATH, PACKAGES_FILE);
        
        if (download_packages_list(url, cache_path) != 0) {
            fprintf(stderr, "Failed to download package list\n");
            return 1;
        }
    }
    
    // Парсим кэш
    parse_packages_file(cache_path);
    
    // Ищем пакет
    struct Package *pkg = find_package(name);
    if (!pkg) {
        fprintf(stderr, "Package '%s' not found.\n\n", name);
        if (pkg_count > 0) {
            fprintf(stderr, "Available packages:\n");
            for (int i = 0; i < pkg_count; i++) {
                fprintf(stderr, "  - %s", packages[i].name);
                if (packages[i].version[0])
                    fprintf(stderr, " (%s)", packages[i].version);
                if (packages[i].size > 0)
                    fprintf(stderr, " [%zu bytes]", packages[i].size);
                fprintf(stderr, "\n");
            }
        }
        return 1;
    }
    
    printf("\n%s\n", pkg->name);
    
    // формируем URL
    char url[512];
    if (pkg->filename[0])
        snprintf(url, sizeof(url), "%s/%s/%s", BASE_URL, BASE_PATH, pkg->filename);
    else
        snprintf(url, sizeof(url), "%s/%s/%s/%s", BASE_URL, BASE_PATH, POOL_DIR, pkg->name);
    
    // временный файл
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.download", cache_dir(), pkg->name);
    mkdir_p(cache_dir());
    
    // скачиваем с прогресс-баром
    printf("Update cache\n");
    if (download_with_progress(url, tmp_path, pkg->size) != 0) {
        fprintf(stderr, "Download failed\n");
        unlink(tmp_path);
        return 1;
    }
    
    // устанавливаем
    char dest_dir[512];
    snprintf(dest_dir, sizeof(dest_dir), "%s/bin", install_prefix());
    mkdir_p(dest_dir);
    
    char dest_path[512];
    snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, pkg->name);
    
    // перемещаем
    if (rename(tmp_path, dest_path) != 0) {
        perror("rename");
        unlink(tmp_path);
        return 1;
    }
    
    // chmod +x
    if (chmod(dest_path, 0755) != 0) {
        perror("chmod");
        fprintf(stderr, "Warning: could not make file executable\n");
    }
    
    printf("\nInstall success!\n");
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
        
        if (unlink(path) == 0) {
            count++;
            printf("=");
            fflush(stdout);
        }
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
    snprintf(url, sizeof(url), "%s%s/%s", BASE_URL, BASE_PATH, PACKAGES_FILE);
    
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
    
    if (!file_exists(cache_path) || file_size(cache_path) < 10) {
        cmd_update();
    }
    
    parse_packages_file(cache_path);
    
    int found = 0;
    printf("\nSearch results for '%s':\n\n", query);
    
    for (int i = 0; i < pkg_count; i++) {
        if (strstr(packages[i].name, query)) {
            printf("  %-20s", packages[i].name);
            if (packages[i].version[0])
                printf("%-10s", packages[i].version);
            if (packages[i].size > 0)
                printf("%8zu bytes", packages[i].size);
            printf("\n");
            found++;
        }
    }
    
    if (found == 0)
        printf("  No packages found\n");
    else
        printf("\nFound %d package(s)\n", found);
    
    return 0;
}

static void usage(const char *prog) {
    printf("WPM - Wilix Package Manager\n\n");
    printf("Usage:\n");
    printf("  %s add <pkg>          Install package\n", prog);
    printf("  %s --list-package     Show raw Packages file\n", prog);
    printf("  %s search <query>     Search for packages\n", prog);
    printf("  %s update             Force cache update\n", prog);
    printf("  %s --delete-cache     Clear local cache\n", prog);
    printf("\nExamples:\n");
    printf("  %s add nano\n", prog);
    printf("  %s search edit\n", prog);
    printf("\nCache: %s\n", cache_dir());
    printf("Install prefix: %s/bin\n", install_prefix());
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    
    signal(SIGPIPE, SIG_IGN);
    
    if (!strcmp(argv[1], "add") && argc == 3)
        return cmd_add(argv[2]);
    else if (!strcmp(argv[1], "--list-package") || !strcmp(argv[1], "list"))
        return cmd_list_packages();
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
