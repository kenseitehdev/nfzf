// ff - NBL Fuzzy Filter (with SSH directory browsing)
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE  // Add this line for macOS qsort_r
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <ctype.h>
#include <locale.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <strings.h>
#include <sys/wait.h>
#include <dirent.h>
#include <limits.h>

#define MAX_LINES 100000
#define MAX_LINE_LEN 2048

#define COLOR_NORMAL     1
#define COLOR_SELECTED   2
#define COLOR_MATCH      3
#define COLOR_STATUS     4
#define COLOR_QUERY      5
#define COLOR_EXECUTABLE 6

typedef enum {
    MODE_NORMAL,
    MODE_INSERT
} Mode;

typedef struct {
    char *lines[MAX_LINES];
    int line_count;
    int *scores;
    int *match_indices;
    int match_count;
    char query[256];
    int query_len;
    int selected;
    int scroll_offset;
    int case_sensitive;
    int exact_match;
    char delimiter;
    int preview_enabled;
    char ssh_host[256];
    char ssh_user[256];
    Mode mode;
    char current_dir[PATH_MAX];
    int is_directory_mode;
    int show_hidden;
    int ssh_mode;
} FuzzyState;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [OPTIONS] < file\n"
        "  cmd | %s [OPTIONS]\n"
        "  %s [OPTIONS] file1 [file2 ...]\n"
        "  %s [OPTIONS] -D [directory]\n"
        "  %s [OPTIONS] -D [user@]host:directory\n"
        "\n"
        "Options:\n"
        "  -h, --help          Show this help\n"
        "  -i                  Case-insensitive matching (default)\n"
        "  -s                  Case-sensitive matching\n"
        "  -e                  Exact match mode\n"
        "  -d DELIM            Use delimiter for multi-column display\n"
        "  -D [DIR]            Directory browsing mode (local or remote)\n"
        "                      Examples: -D /home/user\n"
        "                                -D user@host:/remote/path\n"
        "                                -D (uses current directory)\n"
        "\n"
        "Keybindings:\n"
        "  i                   Enter INSERT mode (type to filter)\n"
        "  ESC                 Enter NORMAL mode / Exit\n"
        "  j/k, Down/Up        Move selection (NORMAL mode)\n"
        "  Ctrl+D/Ctrl+U       Half-page down/up\n"
        "  g/G                 Jump to top/bottom\n"
        "  h                   Go to parent directory (directory mode)\n"
        "  .                   Toggle hidden files (directory mode, works when filter empty)\n"
        "  Enter               Select file / Navigate into directory\n"
        "  Ctrl+C, q           Exit without selection (NORMAL mode)\n"
        "  Backspace           Delete character (INSERT mode)\n"
        "  Ctrl+W              Delete word (INSERT mode)\n"
        "  Ctrl+L              Clear query\n"
        "\n"
        "Modes:\n"
        "  NORMAL              Navigate with j/k, press 'i' to filter\n"
        "  INSERT              Type to filter, ESC to return to NORMAL\n"
        "\n"
        "Directory Mode Visual Indicators:\n"
        "  filename/           Directory\n"
        "  filename*           Executable file\n"
        "  filename            Regular file\n"
        "\n"
        "SSH Remote Directory Browsing:\n"
        "  %s -D user@server:/home/user\n"
        "  Navigate remote directories just like local ones!\n"
        "\n"
        "Reads lines from stdin (pipe) OR from file arguments OR browse directory.\n",
        prog, prog, prog, prog, prog, prog
    );
}


int strip_ansi(const char *str, char *clean, size_t clean_size) {
    if (!str || !clean || clean_size == 0) {
        return -1;
    }
    
    size_t i = 0, j = 0;
    int truncated = 0;
    
    while (str[i]) {
        if (str[i] == '\033') {
            // Bounds check: ensure we can look ahead
            if (!str[i+1]) {
                // ESC at end of string, just skip it
                i++;
                continue;
            }
            
            if (str[i+1] == '[') {
                // CSI (Control Sequence Introducer) - most common
                // Format: ESC [ <parameters> <letter>
                i += 2;
                while (str[i] && !isalpha((unsigned char)str[i])) {
                    i++;
                }
                if (str[i]) i++;  // Skip the final letter
                
            } else if (str[i+1] == ']') {
                // OSC (Operating System Command)
                // Format: ESC ] params BEL or ESC ] params ESC backslash
                i += 2;
                while (str[i] && str[i] != '\007' && str[i] != '\033') {
                    i++;
                }
                if (str[i] == '\007') {
                    i++;  // Skip BEL
                } else if (str[i] == '\033' && str[i+1] == '\\') {
                    i += 2;  // Skip ESC backslash
                }
                
            } else if (str[i+1] == '(' || str[i+1] == ')' || 
                       str[i+1] == '*' || str[i+1] == '+') {
                // Character set selection (G0-G3)
                // Format: ESC ( <char> or ESC ) <char> etc.
                i += 2;
                if (str[i]) i++;
                
            } else if (str[i+1] == '#') {
                // Line attributes (double height/width)
                // Format: ESC # <digit>
                i += 2;
                if (str[i]) i++;
                
            } else if (str[i+1] == '%') {
                // Character set selection
                // Format: ESC % <char>
                i += 2;
                if (str[i]) i++;
                
            } else if (str[i+1] == 'c') {
                // Reset (RIS)
                i += 2;
                
            } else if (str[i+1] == '=' || str[i+1] == '>') {
                // Keypad modes
                i += 2;
                
            } else if (str[i+1] >= '@' && str[i+1] <= '_') {
                // Two-byte escape sequence (Fe sequences)
                i += 2;
                
            } else if (str[i+1] >= '0' && str[i+1] <= '9') {
                // Some terminals use ESC <digit> sequences
                i += 2;
                
            } else {
                // Unknown escape, skip ESC and continue
                i++;
            }
            
        } else {
            // Regular character - copy to output
            if (j < clean_size - 1) {
                clean[j++] = str[i++];
            } else {
                // Output buffer full
                truncated = 1;
                i++;
            }
        }
    }
    
    clean[j] = '\0';
    return truncated ? -1 : (int)j;
}


// Thread-safe version using thread-local storage (C11)
#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#include <threads.h>

static thread_local char tls_buffer[MAX_LINE_LEN];

char* strip_ansi_tls(const char *str) {
    strip_ansi(str, tls_buffer, MAX_LINE_LEN);
    return tls_buffer;
}
#endif


static int fuzzy_score(const char *needle, const char *haystack, int case_sensitive) {
    if (!needle || !*needle) return 1000;

    int n_len = (int)strlen(needle);
    int h_len = (int)strlen(haystack);
    if (n_len > h_len) return -1;

    int score = 0;
    int consecutive = 0;
    int h_idx = 0;

    for (int n_idx = 0; n_idx < n_len; n_idx++) {
        char n_ch = needle[n_idx];
        if (!case_sensitive && n_ch >= 'A' && n_ch <= 'Z') n_ch = (char)(n_ch - 'A' + 'a');

        int found = 0;
        for (; h_idx < h_len; h_idx++) {
            char h_ch = haystack[h_idx];
            if (!case_sensitive && h_ch >= 'A' && h_ch <= 'Z') h_ch = (char)(h_ch - 'A' + 'a');

            if (n_ch == h_ch) {
                found = 1;
                score += 1;

                if (n_idx > 0 && h_idx > 0 && consecutive > 0) {
                    // Cap consecutive bonus to prevent overflow
                    int bonus = 5 * consecutive;
                    if (score > INT_MAX - bonus) {
                        score = INT_MAX;  // Cap at max
                    } else {
                        score += bonus;
                    }
                }
                consecutive++;

                if (h_idx == 0 ||
                    haystack[h_idx - 1] == ' ' ||
                    haystack[h_idx - 1] == '/' ||
                    haystack[h_idx - 1] == '_') {
                    if (score <= INT_MAX - 10) {
                        score += 10;
                    } else {
                        score = INT_MAX;
                    }
                }

                h_idx++;
                break;
            } else {
                consecutive = 0;
            }
        }

        if (!found) return -1;
    }

    score -= (h_len - n_len);
    return score;
}
static int compare_scores(const void *a, const void *b, void *state) {
    FuzzyState *st = (FuzzyState*)state;
    int idx_a = *(const int*)a;
    int idx_b = *(const int*)b;

    int diff = st->scores[idx_b] - st->scores[idx_a];
    if (diff != 0) return diff;

    return (int)strlen(st->lines[idx_a]) - (int)strlen(st->lines[idx_b]);
}

static FuzzyState *g_sort_ctx = NULL;
static int compare_scores_static(const void *a, const void *b) {
    return compare_scores(a, b, g_sort_ctx);
}

#if defined(__APPLE__) || defined(__FreeBSD__)
// BSD-style qsort_r
static int compare_scores_bsd(void *state, const void *a, const void *b) {
    return compare_scores(a, b, state);
}
#elif defined(__linux__) || defined(_GNU_SOURCE)
// GNU-style qsort_r
static int compare_scores_gnu(const void *a, const void *b, void *state) {
    return compare_scores(a, b, state);
}
#endif

static void update_matches(FuzzyState *st) {
    st->match_count = 0;

    if (st->query_len == 0) {
        for (int i = 0; i < st->line_count; i++) {
            st->scores[i] = 1000;
            st->match_indices[st->match_count++] = i;
        }
        st->selected = 0;
        st->scroll_offset = 0;
        return;
    }

    for (int i = 0; i < st->line_count; i++) {
        int score;
        if (st->exact_match) {
            const char *found = strstr(st->lines[i], st->query);
            score = found ? 1000 : -1;
        } else {
            score = fuzzy_score(st->query, st->lines[i], st->case_sensitive);
        }
        st->scores[i] = score;
        if (score >= 0) st->match_indices[st->match_count++] = i;
    }

#if defined(__APPLE__) || defined(__FreeBSD__)
    qsort_r(st->match_indices, st->match_count, sizeof(int), st, compare_scores_bsd);
#elif defined(__linux__) || defined(_GNU_SOURCE)
    qsort_r(st->match_indices, st->match_count, sizeof(int), compare_scores_gnu, st);
#else
    // Fallback to unsafe version if qsort_r not available
    g_sort_ctx = st;
    qsort(st->match_indices, st->match_count, sizeof(int), compare_scores_static);
    g_sort_ctx = NULL;
#endif

    st->selected = 0;
    st->scroll_offset = 0;
}
static void add_line(FuzzyState *st, const char *s) {
    if (st->line_count >= MAX_LINES) return;
    if (!s || !*s) return;
    
    size_t s_len = strlen(s);
    if (s_len >= MAX_LINE_LEN) {
        fprintf(stderr, "Warning: line too long (%zu bytes), truncating\n", s_len);
    }
    
    char clean[MAX_LINE_LEN];
    if (strip_ansi(s, clean, sizeof(clean)) < 0) {
        // Truncation occurred, but we still use what we got
    }
    
    char *duplicated = strdup(clean);
    if (!duplicated) {
        fprintf(stderr, "Warning: failed to allocate memory for line\n");
        return;
    }
    
    st->lines[st->line_count++] = duplicated;
}
static void load_stream(FuzzyState *st, FILE *fp) {
    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), fp) && st->line_count < MAX_LINES) {
        size_t len = strlen(line);
        
        // Check if line was truncated (no newline and buffer full)
        if (len > 0 && len == MAX_LINE_LEN - 1 && line[len - 1] != '\n') {
            // Line was too long, consume rest of line
            int ch;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
                // Discard remaining characters
            }
            fprintf(stderr, "Warning: line truncated (exceeds %d bytes)\n", MAX_LINE_LEN);
        }
        
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        add_line(st, line);
    }
}
static void load_directory(FuzzyState *st, const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "Failed to open directory: %s\n", path);
        return;
    }

    // Clear existing lines for reload
    for (int i = 0; i < st->line_count; i++) {
        free(st->lines[i]);
        st->lines[i] = NULL;
    }
    st->line_count = 0;

    struct dirent *entry;
    int skipped = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (st->line_count >= MAX_LINES) {
            skipped++;
            continue;
        }

        // Always skip .
        if (strcmp(entry->d_name, ".") == 0) continue;

        // Always show .. (parent directory)
        int is_parent = (strcmp(entry->d_name, "..") == 0);

        // Skip hidden files if toggle is off (except ..)
        if (!st->show_hidden && !is_parent && entry->d_name[0] == '.') {
            continue;
        }

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        struct stat st_buf;

        // Special handling for .. - always treat as directory
        if (is_parent) {
            add_line(st, "../");
        } else if (stat(full_path, &st_buf) == 0) {
            if (S_ISDIR(st_buf.st_mode)) {
                char dir_entry[MAX_LINE_LEN];
                snprintf(dir_entry, sizeof(dir_entry), "%s/", entry->d_name);
                add_line(st, dir_entry);
            } else {
                // Check if file is executable
                if (st_buf.st_mode & S_IXUSR) {
                    char exec_entry[MAX_LINE_LEN];
                    snprintf(exec_entry, sizeof(exec_entry), "%s*", entry->d_name);
                    add_line(st, exec_entry);
                } else {
                    add_line(st, entry->d_name);
                }
            }
        }
    }

    closedir(dir);

    if (skipped > 0) {
        fprintf(stderr, "Warning: %d entries skipped (MAX_LINES reached)\n", skipped);
    }
}

#include <string.h>
#include <stddef.h>

static int parse_ssh_path(const char *path, char *user, size_t user_size, 
                          char *host, size_t host_size, 
                          char *remote_path, size_t remote_path_size) {
    if (!path || !user || !host || !remote_path) return 0;
    
    const char *colon = strchr(path, ':');
    if (!colon) return 0;  // Not an SSH path
    
    size_t userhost_len = colon - path;
    if (userhost_len >= 1024) return 0;  // Path too long
    
    char temp[1024];
    memcpy(temp, path, userhost_len);
    temp[userhost_len] = '\0';
    
    const char *rpath = colon + 1;
    
    // Parse user@host or just host
    const char *at = strchr(temp, '@');
    if (at) {
        size_t user_len = at - temp;
        size_t host_len = userhost_len - user_len - 1;
        
        if (user_len >= user_size || host_len >= host_size) return 0;
        
        memcpy(user, temp, user_len);
        user[user_len] = '\0';
        
        memcpy(host, at + 1, host_len);
        host[host_len] = '\0';
    } else {
        if (userhost_len >= host_size) return 0;
        
        user[0] = '\0';  // No user specified
        memcpy(host, temp, userhost_len);
        host[userhost_len] = '\0';
    }
    
    // Copy remote path with bounds checking
    size_t rpath_len = strlen(rpath);
    if (rpath_len >= remote_path_size) return 0;
    
    memcpy(remote_path, rpath, rpath_len);
    remote_path[rpath_len] = '\0';
    
    return 1;
}
// Execute command via SSH and capture output
static FILE *ssh_popen(const char *user, const char *host, const char *command) {
    if (!host || !host[0] || !command || !command[0]) {
        return NULL;
    }
    
    char ssh_cmd[2048];
    int written;

    if (user && user[0]) {
        written = snprintf(ssh_cmd, sizeof(ssh_cmd), "ssh -o ConnectTimeout=10 -o BatchMode=yes %s@%s '%s' 2>&1", 
                          user, host, command);
    } else {
        written = snprintf(ssh_cmd, sizeof(ssh_cmd), "ssh -o ConnectTimeout=10 -o BatchMode=yes %s '%s' 2>&1", 
                          host, command);
    }
    
    if (written < 0 || written >= (int)sizeof(ssh_cmd)) {
        fprintf(stderr, "SSH command too long\n");
        return NULL;
    }

    return popen(ssh_cmd, "r");
}

// Load remote directory listing via SSH
static void load_ssh_directory(FuzzyState *st, const char *path) {
    if (!path || !path[0]) {
        fprintf(stderr, "Invalid remote path\n");
        return;
    }
    
    // Clear existing lines
    for (int i = 0; i < st->line_count; i++) {
        free(st->lines[i]);
        st->lines[i] = NULL;
    }
    st->line_count = 0;

    // Build ls command with options
    char ls_cmd[1024];
    int written;
    if (st->show_hidden) {
        written = snprintf(ls_cmd, sizeof(ls_cmd), 
                "cd '%s' 2>/dev/null && ls -Ap1 --color=never 2>/dev/null || ls -Ap1 2>/dev/null", 
                path);
    } else {
        written = snprintf(ls_cmd, sizeof(ls_cmd), 
                "cd '%s' 2>/dev/null && ls -p1 --color=never 2>/dev/null || ls -p1 2>/dev/null", 
                path);
    }
    
    if (written < 0 || written >= (int)sizeof(ls_cmd)) {
        fprintf(stderr, "Directory path too long\n");
        return;
    }

    FILE *fp = ssh_popen(st->ssh_user, st->ssh_host, ls_cmd);
    if (!fp) {
        fprintf(stderr, "Failed to connect to %s@%s\n", 
                st->ssh_user[0] ? st->ssh_user : "ssh", st->ssh_host);
        return;
    }

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), fp) && st->line_count < MAX_LINES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        
        // Skip . entry
        if (strcmp(line, ".") == 0) continue;
        
        // Check for SSH error messages
        if (strncmp(line, "Permission denied", 17) == 0 ||
            strncmp(line, "Connection refused", 18) == 0 ||
            strncmp(line, "Host key verification failed", 28) == 0) {
            fprintf(stderr, "SSH error: %s\n", line);
            pclose(fp);
            return;
        }

        // Check if it's a directory (ends with /)
        int is_dir = (len > 0 && line[len - 1] == '/');
        
        // For non-directories, check if executable via stat
        if (!is_dir && strcmp(line, "..") != 0) {
            char stat_cmd[1024];
            int stat_written = snprintf(stat_cmd, sizeof(stat_cmd), 
                    "[ -x '%s/%s' ] && echo 'x' || echo 'n'", path, line);
            
            if (stat_written > 0 && stat_written < (int)sizeof(stat_cmd)) {
                FILE *stat_fp = ssh_popen(st->ssh_user, st->ssh_host, stat_cmd);
                if (stat_fp) {
                    char result[4] = {0};
                    if (fgets(result, sizeof(result), stat_fp)) {
                        if (result[0] == 'x') {
                            // Append * for executables
                            if (len < MAX_LINE_LEN - 1) {
                                line[len] = '*';
                                line[len + 1] = '\0';
                            }
                        }
                    }
                    pclose(stat_fp);
                }
            }
        }
        
        add_line(st, line);
    }

    int status = pclose(fp);
    if (status != 0) {
        fprintf(stderr, "SSH command exited with status %d\n", WEXITSTATUS(status));
    }
}

// Load file content from SSH
static int load_ssh_file(FuzzyState *st, const char *path) {
    if (!path || !path[0]) {
        return 0;
    }
    
    char user[256], host[256], remote_path[256];

    if (!parse_ssh_path(path, user, sizeof(user), host, sizeof(host), remote_path, sizeof(remote_path))) {
        return 0;  // Not an SSH path
    }

    // Save SSH info for potential later use
    strncpy(st->ssh_host, host, sizeof(st->ssh_host) - 1);
    st->ssh_host[sizeof(st->ssh_host) - 1] = '\0';
    strncpy(st->ssh_user, user, sizeof(st->ssh_user) - 1);
    st->ssh_user[sizeof(st->ssh_user) - 1] = '\0';

    // Execute 'cat' command via SSH
    char command[512];
    int written = snprintf(command, sizeof(command), "cat '%s'", remote_path);
    if (written < 0 || written >= (int)sizeof(command)) {
        fprintf(stderr, "Remote path too long\n");
        return 0;
    }

    FILE *fp = ssh_popen(user, host, command);
    if (!fp) {
        fprintf(stderr, "Failed to execute SSH command for '%s'\n", path);
        return 0;
    }

    load_stream(st, fp);
    
    int status = pclose(fp);
    if (status != 0) {
        fprintf(stderr, "SSH command failed for '%s'\n", path);
        return 0;
    }
    
    return 1;
}
static int load_files(FuzzyState *st, int argc, char **argv, int first_file_idx) {
    int loaded_any = 0;
    for (int i = first_file_idx; i < argc; i++) {
        const char *path = argv[i];

        // Try SSH path first
        if (strchr(path, ':')) {
            if (load_ssh_file(st, path)) {
                loaded_any = 1;
                if (st->line_count >= MAX_LINES) break;
                continue;
            }
        }

        // Fall back to local file
        FILE *fp = fopen(path, "r");
        if (!fp) {
            fprintf(stderr, "nfzf: failed to open '%s': %s\n", path, strerror(errno));
            continue;
        }
        load_stream(st, fp);
        fclose(fp);
        loaded_any = 1;
        if (st->line_count >= MAX_LINES) break;
    }
    return loaded_any;
}

static void load_stdin(FuzzyState *st) {
    load_stream(st, stdin);
}

static void free_state(FuzzyState *st) {
    for (int i = 0; i < st->line_count; i++) {
        if (st->lines[i]) free(st->lines[i]);
    }
    free(st->scores);
    free(st->match_indices);
}

static void draw_status_bar(FuzzyState *st) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    attron(COLOR_PAIR(COLOR_NORMAL));
    mvhline(max_y - 2, 0, ACS_HLINE, max_x);
    attroff(COLOR_PAIR(COLOR_NORMAL));

    move(max_y - 1, 0);
    clrtoeol();

    attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);

    // Start with NBL NFZF
    mvprintw(max_y - 1, 1, "NBL Fuzzy Filter | ");
    int nbl_len = 19;  // "NBL NFZF | " length

    // Determine mode string and color
    const char *mode_str;
    int mode_color;
    if (st->mode == MODE_INSERT) {
        mode_str = "INSERT";
        mode_color = COLOR_QUERY;  // Cyan/distinctive color
    } else {
        mode_str = "NORMAL";
        mode_color = COLOR_MATCH;  // Yellow/distinctive color
    }

    // Draw mode indicator after NBL NFZF
    attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    attron(COLOR_PAIR(mode_color) | A_BOLD);
    mvprintw(max_y - 1, nbl_len, " [%s]", mode_str);
    attroff(COLOR_PAIR(mode_color) | A_BOLD);
    attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);

    // Calculate position after mode indicator
    int mode_len = (int)strlen(mode_str) + 3;  // [MODE] = mode + 2 brackets + space
    int status_start = nbl_len + mode_len;

    char left[256];
    snprintf(left, sizeof(left), " | %d/%d matches | Mode: %s%s%s%s%s",
             st->match_count > 0 ? st->selected + 1 : 0,
             st->match_count,
             st->exact_match ? "EXACT" : "FUZZY",
             st->case_sensitive ? " (case)" : "",
             st->show_hidden ? " | hidden" : "",
             st->is_directory_mode ? " | dir" : "",
             st->ssh_mode ? " | SSH" : "");

    mvprintw(max_y - 1, status_start, "%s", left);

    if (st->query_len > 0) {
        char right[300];
        snprintf(right, sizeof(right), "Query: %s ", st->query);
        int rx = max_x - (int)strlen(right) - 1;
        if (rx < status_start) rx = status_start;
        mvprintw(max_y - 1, rx, "%s", right);
    } else if (st->is_directory_mode) {
        // Show current directory when no query
        char right[300];
        if (st->ssh_mode) {
            snprintf(right, sizeof(right), "%s@%s:%s ", 
                    st->ssh_user[0] ? st->ssh_user : "ssh",
                    st->ssh_host, st->current_dir);
        } else {
            snprintf(right, sizeof(right), "Dir: %s ", st->current_dir);
        }
        int rx = max_x - (int)strlen(right) - 1;
        if (rx < status_start) rx = status_start;
        mvprintw(max_y - 1, rx, "%s", right);
    }

    attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
}
static void highlight_matches(const char *line, const char *query, int y, int x_start, int max_x, int case_sensitive) {
    if (!query || !*query) {
        mvprintw(y, x_start, "%.*s", max_x - x_start, line);
        return;
    }

    int q_len = (int)strlen(query);
    int l_len = (int)strlen(line);
    int x = x_start;

    int matched[MAX_LINE_LEN] = {0};

    int q_idx = 0;
    for (int l_idx = 0; l_idx < l_len && q_idx < q_len && x < max_x; l_idx++) {
        char q_ch = query[q_idx];
        char l_ch = line[l_idx];

        if (!case_sensitive) {
            if (q_ch >= 'A' && q_ch <= 'Z') q_ch = (char)(q_ch - 'A' + 'a');
            if (l_ch >= 'A' && l_ch <= 'Z') l_ch = (char)(l_ch - 'A' + 'a');
        }

        if (q_ch == l_ch) {
            matched[l_idx] = 1;
            q_idx++;
        }
    }

    for (int i = 0; i < l_len && x < max_x; i++) {
        if (matched[i]) {
            attron(COLOR_PAIR(COLOR_MATCH) | A_BOLD);
            mvaddch(y, x++, line[i]);
            attroff(COLOR_PAIR(COLOR_MATCH) | A_BOLD);
        } else {
            mvaddch(y, x++, line[i]);
        }
    }
}

static void draw_results(FuzzyState *st) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    for (int y = 0; y < max_y - 2; y++) {
        move(y, 0);
        clrtoeol();
    }

    int visible_lines = max_y - 2;

    for (int i = 0; i < visible_lines; i++) {
        int match_idx = st->scroll_offset + i;
        if (match_idx >= st->match_count) break;

        int line_idx = st->match_indices[match_idx];
        const char *line = st->lines[line_idx];

        int is_selected = (match_idx == st->selected);
        int is_executable = (strlen(line) > 0 && line[strlen(line) - 1] == '*');

        if (is_selected) {
            attron(COLOR_PAIR(COLOR_SELECTED) | A_REVERSE | A_BOLD);
            mvhline(i, 0, ' ', max_x);
        } else if (is_executable) {
            attron(COLOR_PAIR(COLOR_EXECUTABLE));
        }

        mvprintw(i, 1, is_selected ? "> " : "  ");
        highlight_matches(line, st->query, i, 3, max_x, st->case_sensitive);

        if (is_selected) {
            attroff(COLOR_PAIR(COLOR_SELECTED) | A_REVERSE | A_BOLD);
        } else if (is_executable) {
            attroff(COLOR_PAIR(COLOR_EXECUTABLE));
        }
    }
}

static void draw_ui(FuzzyState *st) {
    draw_results(st);
    draw_status_bar(st);
    refresh();
}

static void ensure_visible(FuzzyState *st) {
    int max_y = getmaxy(stdscr);
    int visible_lines = max_y - 2;

    if (st->selected < st->scroll_offset) st->scroll_offset = st->selected;
    if (st->selected >= st->scroll_offset + visible_lines)
        st->scroll_offset = st->selected - visible_lines + 1;
}

static void move_up(FuzzyState *st) {
    if (st->selected > 0) { st->selected--; ensure_visible(st); }
}

static void move_down(FuzzyState *st) {
    if (st->selected < st->match_count - 1) { st->selected++; ensure_visible(st); }
}

static void page_up(FuzzyState *st) {
    int max_y = getmaxy(stdscr);
    int half_page = (max_y - 2) / 2;

    st->selected -= half_page;
    if (st->selected < 0) st->selected = 0;
    ensure_visible(st);
}

static void page_down(FuzzyState *st) {
    int max_y = getmaxy(stdscr);
    int half_page = (max_y - 2) / 2;

    st->selected += half_page;
    if (st->selected >= st->match_count) st->selected = st->match_count - 1;
    if (st->selected < 0) st->selected = 0;
    ensure_visible(st);
}

static void jump_top(FuzzyState *st) {
    st->selected = 0;
    st->scroll_offset = 0;
}

static void jump_bottom(FuzzyState *st) {
    st->selected = st->match_count - 1;
    if (st->selected < 0) st->selected = 0;
    ensure_visible(st);
}

static void add_char(FuzzyState *st, char c) {
    if (st->query_len < (int)sizeof(st->query) - 1) {
        st->query[st->query_len++] = c;
        st->query[st->query_len] = '\0';
        update_matches(st);
    }
}

static void delete_char(FuzzyState *st) {
    if (st->query_len > 0) {
        st->query[--st->query_len] = '\0';
        update_matches(st);
    }
}

static void delete_word(FuzzyState *st) {
    while (st->query_len > 0) {
        char c = st->query[st->query_len - 1];
        st->query[--st->query_len] = '\0';
        if (c == ' ' || c == '/' || c == '_' || c == '-') break;
    }
    update_matches(st);
}

static void clear_query(FuzzyState *st) {
    st->query[0] = '\0';
    st->query_len = 0;
    update_matches(st);
}

static void toggle_hidden_files(FuzzyState *st) {
    if (!st->is_directory_mode) return;

    st->show_hidden = !st->show_hidden;

    // DON'T clear the query - keep the current filter
    // Just reload the directory with the new hidden files setting
    
    if (st->ssh_mode) {
        load_ssh_directory(st, st->current_dir);
    } else {
        load_directory(st, st->current_dir);
    }
    update_matches(st);

    // Ensure we're at the top after reload
    st->selected = 0;
    st->scroll_offset = 0;

    // Clear screen to make the change obvious
    clear();
}
static void navigate_directory(FuzzyState *st, const char *selection) {
    if (!st->is_directory_mode) return;
    
    // Check for null or empty selection
    if (!selection || !*selection) return;
    
    char new_path[PATH_MAX];
    
    // Handle parent directory (..)
    if (strcmp(selection, "../") == 0 || strcmp(selection, "..") == 0) {
        // Get parent directory
        char *last_slash = strrchr(st->current_dir, '/');
        if (last_slash && last_slash != st->current_dir) {
            *last_slash = '\0';  // Truncate to parent
            strncpy(new_path, st->current_dir, PATH_MAX - 1);
            *last_slash = '/';  // Restore for copy
        } else {
            strncpy(new_path, "/", PATH_MAX - 1);
        }
    }
    // Handle subdirectory (ends with /)
    else if (selection[strlen(selection) - 1] == '/') {
        // Remove trailing /
        char dirname[MAX_LINE_LEN];
        strncpy(dirname, selection, sizeof(dirname) - 1);
        dirname[sizeof(dirname) - 1] = '\0';  // Ensure null termination
        
        size_t len = strlen(dirname);
        if (len > 0 && dirname[len - 1] == '/') {
            dirname[len - 1] = '\0';
        }
        
        // Build new path - handle root directory to avoid double slash
        if (strcmp(st->current_dir, "/") == 0) {
            snprintf(new_path, sizeof(new_path), "/%s", dirname);
        } else {
            snprintf(new_path, sizeof(new_path), "%s/%s", st->current_dir, dirname);
        }
    }
    // Handle executable (ends with *) or regular file
    else {
        // Regular file or executable - don't navigate, just return it
        return;
    }
    
    // Update current directory and reload
    strncpy(st->current_dir, new_path, PATH_MAX - 1);
    st->current_dir[PATH_MAX - 1] = '\0';
    
    // Clear query when navigating
    st->query[0] = '\0';
    st->query_len = 0;
    
    // Reload directory (local or remote)
    if (st->ssh_mode) {
        load_ssh_directory(st, st->current_dir);
    } else {
        load_directory(st, st->current_dir);
    }
    update_matches(st);
}
static void handle_resize(FuzzyState *st) {
    resizeterm(0, 0);
    ensure_visible(st);
    clear();
    draw_ui(st);
}

static int handle_input(FuzzyState *st, int *running) {
    int ch = getch();

    switch (ch) {
        case ERR:
            return -2;

        case KEY_RESIZE:
            handle_resize(st);
            return -2;
    }

    if (st->mode == MODE_NORMAL) {
        switch (ch) {
            case 'i':
                st->mode = MODE_INSERT;
                break;

            case 'q':
            case 3:   // Ctrl+C
                *running = 0;
                return -1;

            case 27:  // ESC
                *running = 0;
                return -1;

            case '\n':
            case '\r':
            case KEY_ENTER:
                if (st->is_directory_mode && st->match_count > 0 && st->selected < st->match_count) {
                    int line_idx = st->match_indices[st->selected];
                    const char *selection = st->lines[line_idx];
                    
                    // Add safety check
                    if (selection && strlen(selection) > 0 &&
                        (selection[strlen(selection) - 1] == '/' ||
                         strcmp(selection, "..") == 0)) {
                        navigate_directory(st, selection);
                        break;
                    }
                }
                *running = 0;
                return st->selected;

            case 'j':
            case KEY_DOWN:
                move_down(st);
                break;

            case 'k':
            case KEY_UP:
                move_up(st);
                break;

            case 4:  // Ctrl+D
                page_down(st);
                break;

            case 21: // Ctrl+U
                page_up(st);
                break;

            case 12: // Ctrl+L
                clear_query(st);
                break;

            case 'g':
                jump_top(st);
                break;

            case 'G':
                jump_bottom(st);
                break;

            case '.':
                toggle_hidden_files(st);
                break;

            case 'h':
                // Go to parent directory
                if (st->is_directory_mode) {
                    navigate_directory(st, "..");
                }
                break;

            default:
                break;
        }
    } else {  // MODE_INSERT
        switch (ch) {
            case 27:  // ESC
                st->mode = MODE_NORMAL;
                break;

            case '\n':
            case '\r':
            case KEY_ENTER:
                if (st->is_directory_mode && st->match_count > 0 && st->selected < st->match_count) {
                    int line_idx = st->match_indices[st->selected];
                    const char *selection = st->lines[line_idx];

                    // Check if it's a directory
                    if (selection && strlen(selection) > 0 &&
                        (selection[strlen(selection) - 1] == '/' ||
                         strcmp(selection, "..") == 0)) {
                        navigate_directory(st, selection);
                        break;  // Don't exit, stay in the browser
                    }
                }
                *running = 0;
                return st->selected;

            case KEY_BACKSPACE:
            case 127:
            case 8:
                delete_char(st);
                break;

            case 23: // Ctrl+W
                delete_word(st);
                break;

            case 12: // Ctrl+L
                clear_query(st);
                break;

            default:
                if (ch == '.' && st->is_directory_mode && st->query_len == 0) {
                    // If in directory mode and query is empty, toggle hidden files
                    toggle_hidden_files(st);
                } else if (isprint(ch)) {
                    add_char(st, (char)ch);
                }
                break;
        }
    }

    return -2;
}
static int parse_flags(int argc, char **argv, FuzzyState *st) {
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return -1;
        } else if (strcmp(argv[i], "-i") == 0) {
            st->case_sensitive = 0;
        } else if (strcmp(argv[i], "-s") == 0) {
            st->case_sensitive = 1;
        } else if (strcmp(argv[i], "-e") == 0) {
            st->exact_match = 1;
        } else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 < argc) st->delimiter = argv[++i][0];
            else {
                fprintf(stderr, "Error: -d requires an argument\n");
                return -1;
            }
        } else if (strcmp(argv[i], "-D") == 0) {
            st->is_directory_mode = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const char *dir_arg = argv[++i];
                
                // Check if it's an SSH path
                char user[256], host[256], remote_path[256];
                if (parse_ssh_path(dir_arg, user, sizeof(user), host, sizeof(host), remote_path, sizeof(remote_path))) {
                    // SSH directory mode
                    st->ssh_mode = 1;
strncpy(st->ssh_user, user, sizeof(st->ssh_user) - 1);
st->ssh_user[sizeof(st->ssh_user) - 1] = '\0';
strncpy(st->ssh_host, host, sizeof(st->ssh_host) - 1);
st->ssh_host[sizeof(st->ssh_host) - 1] = '\0';
strncpy(st->current_dir, remote_path, PATH_MAX - 1);
st->current_dir[PATH_MAX - 1] = '\0';
                } else {
                    // Local directory mode
                    strncpy(st->current_dir, dir_arg, PATH_MAX - 1);
                }
            } else {
                // No directory specified, use current
                if (!getcwd(st->current_dir, PATH_MAX)) {
        fprintf(stderr, "Error: failed to get current directory: %s\n", strerror(errno));
                    return -1;
                }
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        } else {
            break; // first file arg
        }
    }
    return i;
}
int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
char output[256];
    FuzzyState *st = calloc(1, sizeof(FuzzyState));
    if (!st) {
        fprintf(stderr, "Failed to allocate memory\n");
        return 1;
    }

    st->case_sensitive = 0;
    st->exact_match = 0;
    st->delimiter = '\0';
    st->query[0] = '\0';
    st->query_len = 0;
    st->ssh_host[0] = '\0';
    st->ssh_user[0] = '\0';
    st->mode = MODE_NORMAL;  // Start in NORMAL mode
    st->is_directory_mode = 0;
    st->show_hidden = 0;
    st->ssh_mode = 0;
    st->current_dir[0] = '\0';

    int first_file_idx = parse_flags(argc, argv, st);
    if (first_file_idx < 0) {
        free(st);
        return 0; // help printed
    }

    st->scores = calloc(MAX_LINES, sizeof(int));
    st->match_indices = calloc(MAX_LINES, sizeof(int));
    if (!st->scores || !st->match_indices) {
        fprintf(stderr, "Failed to allocate memory\n");
        free(st->scores);
        free(st->match_indices);
        free(st);
        return 1;
    }

    // Input mode:
    // - If directory mode (-D) => load directory (local or SSH)
    // - If stdin is NOT a TTY => read from stdin (pipe)
    // - If stdin IS a TTY => read from file args (if provided)
    if (st->is_directory_mode) {
        if (st->ssh_mode) {
            load_ssh_directory(st, st->current_dir);
        } else {
            load_directory(st, st->current_dir);
        }
    } else if (!isatty(STDIN_FILENO)) {
        load_stdin(st);
    } else {
        if (first_file_idx >= argc) {
            fprintf(stderr, "nfzf: no piped input and no files provided.\n\n");
            usage(argv[0]);
            free_state(st);
            free(st);
            return 1;
        }
        int ok = load_files(st, argc, argv, first_file_idx);
        if (!ok) {
            fprintf(stderr, "nfzf: no readable input files.\n");
            free_state(st);
            free(st);
            return 1;
        }
    }

    if (st->line_count == 0) {
        fprintf(stderr, "No input lines\n");
        free_state(st);
        free(st);
        return 1;
    }

    update_matches(st);

    // Open controlling TTY for interactive UI even when stdin is a pipe.
    FILE *tty_in  = fopen("/dev/tty", "r");
    FILE *tty_out = fopen("/dev/tty", "w");
    if (!tty_in || !tty_out) {
        fprintf(stderr, "nfzf: failed to open /dev/tty for interactive input/output\n");
        if (tty_in) fclose(tty_in);
        if (tty_out) fclose(tty_out);
        free_state(st);
        free(st);
        return 1;
    }

    SCREEN *scr = newterm(NULL, tty_out, tty_in);
    if (!scr) {
        fprintf(stderr, "nfzf: newterm() failed\n");
        fclose(tty_in);
        fclose(tty_out);
        free_state(st);
        free(st);
        return 1;
    }
    set_term(scr);

#if defined(NCURSES_VERSION)
    set_escdelay(25);
#endif

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(-1); // blocking

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COLOR_NORMAL,     COLOR_WHITE,   -1);
        init_pair(COLOR_SELECTED,   COLOR_WHITE,   -1);
        init_pair(COLOR_MATCH,      COLOR_YELLOW,  -1);
        init_pair(COLOR_STATUS,     COLOR_WHITE,   -1);
        init_pair(COLOR_QUERY,      COLOR_CYAN,    -1);
        init_pair(COLOR_EXECUTABLE, COLOR_GREEN,   -1);
    }

    int running = 1;
    int result = -1;

    while (running) {
        draw_ui(st);
        result = handle_input(st, &running);
    }

    endwin();
    delscreen(scr);
    fclose(tty_in);
    fclose(tty_out);

    if (result >= 0 && result < st->match_count) {
        int line_idx = st->match_indices[result];
        const char *selected = st->lines[line_idx];

        // Strip trailing / or * markers for output
        strncpy(output, selected, sizeof(output) - 1);
        output[sizeof(output) - 1] = '\0';

        size_t len = strlen(output);
        if (len > 0 && (output[len - 1] == '/' || output[len - 1] == '*')) {
            output[len - 1] = '\0';
        }

        // In directory mode, output full path
        if (st->is_directory_mode) {
            // Don't include .. in the full path output
            if (strcmp(output, "..") == 0) {
                if (st->ssh_mode) {
                    // For SSH, output the connection string with current dir
                    if (st->ssh_user[0]) {
                        printf("%s@%s:%s\n", st->ssh_user, st->ssh_host, st->current_dir);
                    } else {
                        printf("%s:%s\n", st->ssh_host, st->current_dir);
                    }
                } else {
                    printf("%s\n", st->current_dir);
                }
            } else {
                if (st->ssh_mode) {
                    // For SSH, output user@host:/full/path format
                    // Handle trailing slash in current_dir to avoid double slashes
                    const char *separator = (st->current_dir[strlen(st->current_dir) - 1] == '/') ? "" : "/";
                    if (st->ssh_user[0]) {
                        printf("%s@%s:%s%s%s\n", st->ssh_user, st->ssh_host, st->current_dir, separator, output);
                    } else {
                        printf("%s:%s%s%s\n", st->ssh_host, st->current_dir, separator, output);
                    }
                } else {
                    char full_path[PATH_MAX];
                    // Handle trailing slash in current_dir to avoid double slashes
                    const char *separator = (st->current_dir[strlen(st->current_dir) - 1] == '/') ? "" : "/";
                    snprintf(full_path, sizeof(full_path), "%s%s%s", st->current_dir, separator, output);
                    printf("%s\n", full_path);
                }
            }
        } else {
            printf("%s\n", output);
        }
    }

    free_state(st);
    free(st);

    return (result >= 0) ? 0 : 1;
}