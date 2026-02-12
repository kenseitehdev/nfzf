// nfzf - NBL Fuzzy Finder
#define _POSIX_C_SOURCE 200809L
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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
} FuzzyState;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [OPTIONS] < file\n"
        "  cmd | %s [OPTIONS]\n"
        "  %s [OPTIONS] file1 [file2 ...]\n"
        "  %s [OPTIONS] -D [directory]\n"
        "\n"
        "Options:\n"
        "  -h, --help          Show this help\n"
        "  -i                  Case-insensitive matching (default)\n"
        "  -s                  Case-sensitive matching\n"
        "  -e                  Exact match mode\n"
        "  -d DELIM            Use delimiter for multi-column display\n"
        "  -D [DIR]            Directory browsing mode (default: current dir)\n"
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
        "Reads lines from stdin (pipe) OR from file arguments OR browse directory.\n",
        prog, prog, prog, prog
    );
}

static char* strip_ansi(const char *str) {
    static char clean[MAX_LINE_LEN];
    int i = 0, j = 0;

    while (str[i] && j < MAX_LINE_LEN - 1) {
        if (str[i] == '\033') {
            // Handle various ANSI escape sequences
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
            } else if (str[i+1] == '(' || str[i+1] == ')') {
                // Character set selection
                i += 2;
                if (str[i]) i++;
            } else if (str[i+1] >= '@' && str[i+1] <= '_') {
                // Two-byte sequence
                i += 2;
            } else {
                // Unknown escape, skip ESC and continue
                i++;
            }
        } else {
            clean[j++] = str[i++];
        }
    }
    clean[j] = '\0';
    return clean;
}

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
                    score += 5 * consecutive;
                }
                consecutive++;

                if (h_idx == 0 ||
                    haystack[h_idx - 1] == ' ' ||
                    haystack[h_idx - 1] == '/' ||
                    haystack[h_idx - 1] == '_') {
                    score += 10;
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

static void update_matches(FuzzyState *st) {
    st->match_count = 0;

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

    g_sort_ctx = st;
    qsort(st->match_indices, st->match_count, sizeof(int), compare_scores_static);
    g_sort_ctx = NULL;

    st->selected = 0;
    st->scroll_offset = 0;
}

static void add_line(FuzzyState *st, const char *s) {
    if (st->line_count >= MAX_LINES) return;
    if (!s || !*s) return;

    const char *clean = strip_ansi(s);
    st->lines[st->line_count++] = strdup(clean);
}

static void load_stream(FuzzyState *st, FILE *fp) {
    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), fp) && st->line_count < MAX_LINES) {
        size_t len = strlen(line);
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

static int parse_ssh_path(const char *path, char *user, char *host, char *remote_path) {
    if (!strchr(path, ':')) return 0;  // Not an SSH path

    char temp[1024];
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    char *colon = strchr(temp, ':');
    if (!colon) return 0;

    *colon = '\0';
    char *userhost = temp;
    char *rpath = colon + 1;

    // Parse user@host or just host
    char *at = strchr(userhost, '@');
    if (at) {
        *at = '\0';
        strncpy(user, userhost, 255);
        strncpy(host, at + 1, 255);
    } else {
        user[0] = '\0';  // No user specified
        strncpy(host, userhost, 255);
    }

    strncpy(remote_path, rpath, 255);
    return 1;
}

// Execute command via SSH and capture output
static FILE *ssh_popen(const char *user, const char *host, const char *command) {
    char ssh_cmd[2048];

    if (user && user[0]) {
        snprintf(ssh_cmd, sizeof(ssh_cmd), "ssh %s@%s '%s'", user, host, command);
    } else {
        snprintf(ssh_cmd, sizeof(ssh_cmd), "ssh %s '%s'", host, command);
    }

    return popen(ssh_cmd, "r");
}

// Load file content from SSH
static int load_ssh_file(FuzzyState *st, const char *path) {
    char user[256], host[256], remote_path[256];

    if (!parse_ssh_path(path, user, host, remote_path)) {
        return 0;  // Not an SSH path
    }

    // Save SSH info for potential later use
    strncpy(st->ssh_host, host, sizeof(st->ssh_host) - 1);
    strncpy(st->ssh_user, user, sizeof(st->ssh_user) - 1);

    // Execute 'cat' command via SSH
    char command[512];
    snprintf(command, sizeof(command), "cat '%s'", remote_path);

    FILE *fp = ssh_popen(user, host, command);
    if (!fp) {
        fprintf(stderr, "nfzf: failed to execute SSH command for '%s'\n", path);
        return 0;
    }

    load_stream(st, fp);
    pclose(fp);
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
    mvprintw(max_y - 1, 1, "NBL FF | ");
    int nbl_len = 11;  // "NBL NFZF | " length

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
    mvprintw(max_y - 1, nbl_len, "[%s]", mode_str);
    attroff(COLOR_PAIR(mode_color) | A_BOLD);
    attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);

    // Calculate position after mode indicator
    int mode_len = (int)strlen(mode_str) + 3;  // [MODE] = mode + 2 brackets + space
    int status_start = nbl_len + mode_len;

    char left[256];
    snprintf(left, sizeof(left), " | %d/%d matches | Mode: %s%s%s%s",
             st->match_count > 0 ? st->selected + 1 : 0,
             st->match_count,
             st->exact_match ? "EXACT" : "FUZZY",
             st->case_sensitive ? " (case)" : "",
             st->show_hidden ? " | hidden" : "",
             st->is_directory_mode ? " | dir" : "");

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
        snprintf(right, sizeof(right), "Dir: %s ", st->current_dir);
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

    // Clear query to show all files after toggle
    st->query[0] = '\0';
    st->query_len = 0;

    load_directory(st, st->current_dir);
    update_matches(st);

    // Ensure we're at the top after reload
    st->selected = 0;
    st->scroll_offset = 0;

    // Clear screen to make the change obvious
    clear();
}

static void navigate_directory(FuzzyState *st, const char *selection) {
    if (!st->is_directory_mode) return;

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
        dirname[strlen(dirname) - 1] = '\0';

        // Build new path
        snprintf(new_path, sizeof(new_path), "%s/%s", st->current_dir, dirname);
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

    // Reload directory
    load_directory(st, st->current_dir);
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
                if (st->is_directory_mode && st->match_count > 0) {
                    // Get selected line
                    int line_idx = st->match_indices[st->selected];
                    const char *selection = st->lines[line_idx];

                    // Check if it's a directory
                    if (selection[strlen(selection) - 1] == '/' ||
                        strcmp(selection, "..") == 0) {
                        navigate_directory(st, selection);
                        break;  // Don't exit, stay in the browser
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
                if (st->is_directory_mode && st->match_count > 0) {
                    // Get selected line
                    int line_idx = st->match_indices[st->selected];
                    const char *selection = st->lines[line_idx];

                    // Check if it's a directory
                    if (selection[strlen(selection) - 1] == '/' ||
                        strcmp(selection, "..") == 0) {
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

// Parse options and return index of first non-option argument (files).
// Supports `--` to end option parsing.
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
                strncpy(st->current_dir, argv[++i], PATH_MAX - 1);
            } else {
                if (!getcwd(st->current_dir, PATH_MAX)) {
                    fprintf(stderr, "Error: failed to get current directory\n");
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
    // - If directory mode (-D) => load directory
    // - If stdin is NOT a TTY => read from stdin (pipe)
    // - If stdin IS a TTY => read from file args (if provided)
    if (st->is_directory_mode) {
        load_directory(st, st->current_dir);
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
        char output[MAX_LINE_LEN];
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
                printf("%s\n", st->current_dir);
            } else {
                char full_path[PATH_MAX];
                snprintf(full_path, sizeof(full_path), "%s/%s", st->current_dir, output);
                printf("%s\n", full_path);
            }
        } else {
            printf("%s\n", output);
        }
    }

    free_state(st);
    free(st);

    return (result >= 0) ? 0 : 1;
}