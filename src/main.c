// nfzf - NBL Fuzzy Finder (termios version for embedded)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _DARWIN_C_SOURCE
#include <signal.h>

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <locale.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <strings.h>
#include <fcntl.h>      // for open(), O_RDWR
#define MAX_LINES 10000
#define MAX_LINE_LEN 2048

// ANSI escape sequences
#define ESC "\x1b"
#define CSI ESC "["

#define CLEAR_SCREEN CSI "2J"
#define CURSOR_HOME CSI "H"
#define CURSOR_HIDE CSI "?25l"
#define CURSOR_SHOW CSI "?25h"
#define CLEAR_LINE CSI "2K"
#define SAVE_CURSOR ESC "7"
#define RESTORE_CURSOR ESC "8"

// Colors
#define COLOR_RESET CSI "0m"
#define COLOR_BOLD CSI "1m"
#define COLOR_REVERSE CSI "7m"
#define COLOR_YELLOW CSI "33m"
#define COLOR_CYAN CSI "36m"
#define COLOR_WHITE CSI "37m"

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
    char ssh_host[256];
    char ssh_user[256];
} FuzzyState;

// Terminal state
static struct termios orig_termios;
static int term_rows = 24;
static int term_cols = 80;
static volatile sig_atomic_t winch_received = 0;

// Signal handler for window resize
static void sigwinch_handler(int sig) {
    (void)sig;
    winch_received = 1;
}

// Get terminal size
static void update_term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        term_rows = ws.ws_row;
        term_cols = ws.ws_col;
    }
}

// Restore terminal to original state
static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    write(STDOUT_FILENO, CURSOR_SHOW, strlen(CURSOR_SHOW));
    write(STDOUT_FILENO, COLOR_RESET, strlen(COLOR_RESET));
}

// Enable raw mode for character-by-character input
static void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;   // Return immediately
    raw.c_cc[VTIME] = 1;  // 100ms timeout

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Move cursor to position (1-indexed like ncurses)
static void move_cursor(int row, int col) {
    char buf[32];
    snprintf(buf, sizeof(buf), CSI "%d;%dH", row + 1, col + 1);
    write(STDOUT_FILENO, buf, strlen(buf));
}

// Clear from cursor to end of line
static void clear_to_eol(void) {
    write(STDOUT_FILENO, CSI "K", 3);
}

// Read a single key, handling escape sequences
static int read_key(void) {
    char c;
    while (read(STDIN_FILENO, &c, 1) == 0) {
        // Check for resize signal
        if (winch_received) {
            winch_received = 0;
            update_term_size();
            return -1000;  // Special code for resize
        }
    }

    if (c != '\x1b') return c;

    // Escape sequence - read next chars with timeout
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            // Extended sequence
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
            if (seq[2] == '~') {
                switch (seq[1]) {
                    case '1': return 1000; // Home
                    case '3': return 1001; // Delete
                    case '4': return 1002; // End
                    case '5': return 1003; // Page Up
                    case '6': return 1004; // Page Down
                }
            }
        } else {
            switch (seq[1]) {
                case 'A': return 1010; // Up arrow
                case 'B': return 1011; // Down arrow
                case 'C': return 1012; // Right arrow
                case 'D': return 1013; // Left arrow
                case 'H': return 1000; // Home
                case 'F': return 1002; // End
            }
        }
    }

    return '\x1b';
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [OPTIONS] < file\n"
        "  cmd | %s [OPTIONS]\n"
        "  %s [OPTIONS] file1 [file2 ...]\n"
        "\n"
        "Options:\n"
        "  -h, --help          Show this help\n"
        "  -i                  Case-insensitive matching (default)\n"
        "  -s                  Case-sensitive matching\n"
        "  -e                  Exact match mode\n"
        "  -d DELIM            Use delimiter for multi-column display\n"
        "\n"
        "Keybindings:\n"
        "  j/k, Down/Up        Move selection\n"
        "  Ctrl+D/Ctrl+U       Half-page down/up\n"
        "  g/G                 Jump to top/bottom\n"
        "  Enter               Select and exit\n"
        "  Ctrl+C, Esc, q      Exit without selection\n"
        "  Type to filter      Fuzzy search\n"
        "  Backspace           Delete character\n"
        "  Ctrl+W              Delete word\n"
        "  Ctrl+L              Clear query\n"
        "\n"
        "Reads lines from stdin (pipe) OR from file arguments.\n",
        prog, prog, prog
    );
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

static FuzzyState *g_sort_ctx = NULL;

static int compare_scores(const void *a, const void *b) {
    FuzzyState *st = g_sort_ctx;
    int idx_a = *(const int*)a;
    int idx_b = *(const int*)b;

    int diff = st->scores[idx_b] - st->scores[idx_a];
    if (diff != 0) return diff;

    return (int)strlen(st->lines[idx_a]) - (int)strlen(st->lines[idx_b]);
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
    qsort(st->match_indices, st->match_count, sizeof(int), compare_scores);
    g_sort_ctx = NULL;

    st->selected = 0;
    st->scroll_offset = 0;
}

static void add_line(FuzzyState *st, const char *s) {
    if (st->line_count >= MAX_LINES) return;
    if (!s || !*s) return;

    st->lines[st->line_count++] = strdup(s);
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

static int parse_ssh_path(const char *path, char *user, char *host, char *remote_path) {
    if (!strchr(path, ':')) return 0;

    char temp[1024];
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    char *colon = strchr(temp, ':');
    if (!colon) return 0;

    *colon = '\0';
    char *userhost = temp;
    char *rpath = colon + 1;

    char *at = strchr(userhost, '@');
    if (at) {
        *at = '\0';
        strncpy(user, userhost, 255);
        strncpy(host, at + 1, 255);
    } else {
        user[0] = '\0';
        strncpy(host, userhost, 255);
    }

    strncpy(remote_path, rpath, 255);
    return 1;
}

static FILE *ssh_popen(const char *user, const char *host, const char *command) {
    char ssh_cmd[2048];

    if (user && user[0]) {
        snprintf(ssh_cmd, sizeof(ssh_cmd), "ssh %s@%s '%s'", user, host, command);
    } else {
        snprintf(ssh_cmd, sizeof(ssh_cmd), "ssh %s '%s'", host, command);
    }

    return popen(ssh_cmd, "r");
}

static int load_ssh_file(FuzzyState *st, const char *path) {
    char user[256], host[256], remote_path[256];

    if (!parse_ssh_path(path, user, host, remote_path)) {
        return 0;
    }

    strncpy(st->ssh_host, host, sizeof(st->ssh_host) - 1);
    strncpy(st->ssh_user, user, sizeof(st->ssh_user) - 1);

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

        if (strchr(path, ':')) {
            if (load_ssh_file(st, path)) {
                loaded_any = 1;
                if (st->line_count >= MAX_LINES) break;
                continue;
            }
        }

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
    for (int i = 0; i < st->line_count; i++) free(st->lines[i]);
    free(st->scores);
    free(st->match_indices);
}

// Write string to stdout
static void write_str(const char *s) {
    write(STDOUT_FILENO, s, strlen(s));
}

static void draw_status_bar(FuzzyState *st) {
    // Draw horizontal line at second-to-last row
    move_cursor(term_rows - 2, 0);
    write_str(COLOR_RESET);
    for (int i = 0; i < term_cols; i++) {
        write(STDOUT_FILENO, "-", 1);
    }

    // Status line at bottom
    move_cursor(term_rows - 1, 0);
    clear_to_eol();
    write_str(COLOR_WHITE COLOR_BOLD);

    char left[256];
    snprintf(left, sizeof(left), "NBL NFZF | %d/%d matches | Mode: %s%s",
             st->match_count > 0 ? st->selected + 1 : 0,
             st->match_count,
             st->exact_match ? "EXACT" : "FUZZY",
             st->case_sensitive ? " (case)" : "");

    write_str(left);

    if (st->query_len > 0) {
        char right[300];
        snprintf(right, sizeof(right), "Query: %s ", st->query);
        int rx = term_cols - (int)strlen(right) - 1;
        if (rx > (int)strlen(left)) {
            move_cursor(term_rows - 1, rx);
            write_str(right);
        }
    }

    write_str(COLOR_RESET);
}

static void highlight_matches(const char *line, const char *query, int row, int x_start, int case_sensitive) {
    move_cursor(row, x_start);

    if (!query || !*query) {
        // No query, just print
        int max_len = term_cols - x_start;
        if (max_len > 0) {
            char buf[MAX_LINE_LEN];
            snprintf(buf, max_len + 1, "%s", line);
            write_str(buf);
        }
        return;
    }

    int q_len = (int)strlen(query);
    int l_len = (int)strlen(line);
    int matched[MAX_LINE_LEN] = {0};

    // Mark matched characters
    int q_idx = 0;
    for (int l_idx = 0; l_idx < l_len && q_idx < q_len; l_idx++) {
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

    // Print with highlights
    int x = x_start;
    for (int i = 0; i < l_len && x < term_cols; i++) {
        if (matched[i]) {
            write_str(COLOR_YELLOW COLOR_BOLD);
            write(STDOUT_FILENO, &line[i], 1);
            write_str(COLOR_RESET);
        } else {
            write(STDOUT_FILENO, &line[i], 1);
        }
        x++;
    }
}

static void draw_results(FuzzyState *st) {
    // Clear all lines except status bar
    for (int y = 0; y < term_rows - 2; y++) {
        move_cursor(y, 0);
        clear_to_eol();
    }

    int visible_lines = term_rows - 2;

    for (int i = 0; i < visible_lines; i++) {
        int match_idx = st->scroll_offset + i;
        if (match_idx >= st->match_count) break;

        int line_idx = st->match_indices[match_idx];
        const char *line = st->lines[line_idx];

        int is_selected = (match_idx == st->selected);

        move_cursor(i, 0);

        if (is_selected) {
            write_str(COLOR_REVERSE COLOR_BOLD);
            // Fill entire line with reverse video
            for (int x = 0; x < term_cols; x++) {
                write(STDOUT_FILENO, " ", 1);
            }
            move_cursor(i, 1);
        }

        write_str(is_selected ? "> " : "  ");
        highlight_matches(line, st->query, i, 3, st->case_sensitive);

        if (is_selected) {
            write_str(COLOR_RESET);
        }
    }
}

static void draw_ui(FuzzyState *st) {
    // Don't clear screen on every draw - just overwrite changed areas
    draw_results(st);
    draw_status_bar(st);
}

static void ensure_visible(FuzzyState *st) {
    int visible_lines = term_rows - 2;

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
    int half_page = (term_rows - 2) / 2;
    st->selected -= half_page;
    if (st->selected < 0) st->selected = 0;
    ensure_visible(st);
}

static void page_down(FuzzyState *st) {
    int half_page = (term_rows - 2) / 2;
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

static void handle_resize(FuzzyState *st) {
    update_term_size();
    ensure_visible(st);
    write_str(CLEAR_SCREEN);
    draw_ui(st);
}

static int handle_input(FuzzyState *st, int *running) {
    int ch = read_key();

    switch (ch) {
        case -1000:  // Window resize
            handle_resize(st);
            break;

        case 'q':
        case 3:   // Ctrl+C
        case 27:  // ESC
            *running = 0;
            return -1;

        case '\n':
        case '\r':
            *running = 0;
            return st->selected;

        case 'j':
        case 1011:  // Down arrow
            move_down(st);
            break;

        case 'k':
        case 1010:  // Up arrow
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

        case 127:  // Backspace
        case 8:    // Ctrl+H
            delete_char(st);
            break;

        case 23: // Ctrl+W
            delete_word(st);
            break;

        default:
            if (isprint(ch)) add_char(st, (char)ch);
            break;
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
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        } else {
            break;
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

    int first_file_idx = parse_flags(argc, argv, st);
    if (first_file_idx < 0) {
        free(st);
        return 0;
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

    // Load input
    if (!isatty(STDIN_FILENO)) {
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

    // Open /dev/tty for interactive I/O
    int tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd < 0) {
        fprintf(stderr, "nfzf: failed to open /dev/tty\n");
        free_state(st);
        free(st);
        return 1;
    }

    // Redirect stdin/stdout to tty
    int saved_stdin = dup(STDIN_FILENO);
    int saved_stdout = dup(STDOUT_FILENO);
    dup2(tty_fd, STDIN_FILENO);
    dup2(tty_fd, STDOUT_FILENO);
    close(tty_fd);

    // Setup terminal
    update_term_size();
    
    // Install signal handler for window resize
    struct sigaction sa;
    sa.sa_handler = sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);

    enable_raw_mode();
    
    write_str(CURSOR_HIDE);
    write_str(CLEAR_SCREEN);
    write_str(CURSOR_HOME);

    int running = 1;
    int result = -1;

    while (running) {
        draw_ui(st);
        result = handle_input(st, &running);
    }

    // Cleanup
    write_str(CLEAR_SCREEN);
    write_str(CURSOR_HOME);
    write_str(CURSOR_SHOW);
    write_str(COLOR_RESET);
    
    disable_raw_mode();

    // Restore original stdin/stdout
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdin);
    close(saved_stdout);

    if (result >= 0 && result < st->match_count) {
        int line_idx = st->match_indices[result];
        printf("%s\n", st->lines[line_idx]);
    }

    free_state(st);
    free(st);

    return (result >= 0) ? 0 : 1;
}