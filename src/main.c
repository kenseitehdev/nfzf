// ff fuzzy filter
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE


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
#include <regex.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_LINE_LEN 2048
#define MAX_LINES 2000

#define COLOR_NORMAL     1
#define COLOR_SELECTED   2
#define COLOR_MATCH      3
#define COLOR_STATUS     4
#define COLOR_QUERY      5
#define COLOR_EXECUTABLE 6
#define COLOR_ERROR      7

#define ANSI_PAIR_BASE  20

typedef enum {
    MODE_NORMAL,
    MODE_INSERT
} Mode;

typedef enum {
    MATCH_FUZZY,
    MATCH_EXACT,
    MATCH_REGEX
} MatchMode;

typedef struct {
    char *lines[MAX_LINES];
    char *raw_lines[MAX_LINES];
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

    MatchMode match_mode;
    regex_t regex;
    int regex_valid;
    char regex_error[256];

    char **source_files;
    int *line_numbers;
    int source_file_count;
    int grep_mode;

    char **input_files;
    int input_file_count;
    int from_stdin;

    int   live_mode;
    char *live_cmd;
    int   live_interval_ms;
    long  last_live_refresh_ms;

    int ansi_render;
} FuzzyState;

static void load_stream(FuzzyState *st, FILE *fp);
static void update_matches(FuzzyState *st);
static void ensure_visible(FuzzyState *st);

static long now_ms(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)(tv.tv_sec * 1000L + tv.tv_usec / 1000L);
#endif
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [OPTIONS] < file\n"
        "  cmd | %s [OPTIONS]\n"
        "  %s [OPTIONS] file1 [file2 ...]\n"
        "  %s [OPTIONS] -D [directory]\n"
        "  %s [OPTIONS] -D [user@]host:directory\n"
        "  %s [OPTIONS] -G file1 [file2 ...]\n"
        "  --live CMD          Live mode: rerun CMD periodically and refresh results\n"
        "  --interval MS       Live refresh interval in milliseconds (default 1000)\n"
        "\n"
        "Options:\n"
        "  -h, --help          Show this help\n"
        "  -i                  Case-insensitive matching (default)\n"
        "  -s                  Case-sensitive matching\n"
        "  -e                  Start in exact match mode\n"
        "  -r                  Start in regex match mode\n"
        "  -d DELIM            Use delimiter for multi-column display\n"
        "  -D [DIR]            Directory browsing mode (local or remote)\n"
        "  -G                  Grep mode - show filename:line_number:content\n"
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
        "  Ctrl+R              Refresh source (reload files/directory)\n"
        "  Ctrl+E              Toggle EXACT match mode\n"
        "  Ctrl+F              Toggle FUZZY match mode\n"
        "  Ctrl+X              Toggle REGEX match mode\n"
        "\n"
        "Reads lines from stdin (pipe) OR from file arguments OR browse directory.\n",
        prog, prog, prog, prog, prog, prog
    );
}

static int strip_ansi(const char *in, char *out, size_t out_cap)
{
    if (!in || !out || out_cap == 0) return -1;

    size_t i = 0, j = 0;

    while (in[i]) {
        unsigned char c = (unsigned char)in[i];

        if (c == 0x1B) {
            unsigned char n = (unsigned char)in[i + 1];
            if (!n) { i++; continue; }

            if (n == '[') {
                i += 2;
                while (in[i]) {
                    unsigned char b = (unsigned char)in[i];
                    if (b >= 0x40 && b <= 0x7E) { i++; break; }
                    i++;
                }
                continue;
            }

            if (n == ']') {
                i += 2;
                while (in[i]) {
                    if (in[i] == '\a') { i++; break; }
                    if ((unsigned char)in[i] == 0x1B && in[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
                continue;
            }

            if (n == 'P' || n == 'X' || n == '^' || n == '_') {
                i += 2;
                while (in[i]) {
                    if ((unsigned char)in[i] == 0x1B && in[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
                continue;
            }

            i += 2;
            continue;
        }

        if (j + 1 < out_cap)
            out[j++] = (char)c;

        i++;
    }

    out[j] = '\0';
    return (int)j;
}

static int has_ansi_escape(const char *s) {
    if (!s) return 0;
    return strchr(s, '\033') != NULL;
}

static void build_matched_mask(const char *plain, const char *query, int case_sensitive,
                               int *out_mask, int mask_cap) {
    if (!out_mask || mask_cap <= 0) return;
    memset(out_mask, 0, (size_t)mask_cap * sizeof(int));

    if (!plain || !*plain) return;
    if (!query || !*query) return;

    int q_len = (int)strlen(query);
    int l_len = (int)strlen(plain);
    if (l_len > mask_cap) l_len = mask_cap;

    int q_idx = 0;
    for (int l_idx = 0; l_idx < l_len && q_idx < q_len; l_idx++) {
        char q_ch = query[q_idx];
        char l_ch = plain[l_idx];

        if (!case_sensitive) {
            if (q_ch >= 'A' && q_ch <= 'Z') q_ch = (char)(q_ch - 'A' + 'a');
            if (l_ch >= 'A' && l_ch <= 'Z') l_ch = (char)(l_ch - 'A' + 'a');
        }

        if (q_ch == l_ch) {
            out_mask[l_idx] = 1;
            q_idx++;
        }
    }
}

typedef struct {
    int bold;
    int underline;
    int reverse;
    int fg;      // 0..7 or -1 default
    int bright;  // 0/1
} AnsiStyle;

static void ansi_apply_style(const AnsiStyle *s, attr_t base_attr, short base_pair) {
    attr_t a = base_attr;
    short pair = base_pair;

    if (s) {
        if (s->bold) a |= A_BOLD;
        if (s->underline) a |= A_UNDERLINE;
        if (s->reverse) a |= A_REVERSE;

        if (s->fg >= 0 && s->fg <= 7) {
            pair = (short)(ANSI_PAIR_BASE + s->fg + (s->bright ? 8 : 0));
        }
    }

    attrset(a | COLOR_PAIR(pair));
}

static void xterm256_to_rgb(int n, int *r, int *g, int *b) {
    if (r) *r = 255;
    if (g) *g = 255;
    if (b) *b = 255;

    if (n < 0) n = 0;
    if (n > 255) n = 255;

    if (n < 16) {
        static const int base[16][3] = {
            {0,0,0},{205,0,0},{0,205,0},{205,205,0},
            {0,0,238},{205,0,205},{0,205,205},{229,229,229},
            {127,127,127},{255,0,0},{0,255,0},{255,255,0},
            {92,92,255},{255,0,255},{0,255,255},{255,255,255}
        };
        if (r) *r = base[n][0];
        if (g) *g = base[n][1];
        if (b) *b = base[n][2];
        return;
    }

    if (n >= 16 && n <= 231) {
        int x = n - 16;
        int rr = x / 36;
        int gg = (x / 6) % 6;
        int bb = x % 6;
        static const int steps[6] = {0, 95, 135, 175, 215, 255};
        if (r) *r = steps[rr];
        if (g) *g = steps[gg];
        if (b) *b = steps[bb];
        return;
    }

    // grayscale 232..255
    int shade = 8 + (n - 232) * 10;
    if (r) *r = shade;
    if (g) *g = shade;
    if (b) *b = shade;
}

static void rgb_to_ansi16(int r, int g, int b, int *fg, int *bright) {
    // Map RGB to 8 base hues + brightness. Not perfect, but makes bat readable.
    int br = (r + g + b) / 3;
    int is_bright = (br >= 160);

    int maxv = r;
    int which = 0; // 0=R,1=G,2=B
    if (g > maxv) { maxv = g; which = 1; }
    if (b > maxv) { maxv = b; which = 2; }

    int minv = r;
    if (g < minv) minv = g;
    if (b < minv) minv = b;

    int sat = maxv - minv;

    int col = 7; // default white-ish
    if (sat < 30) {
        // gray
        col = 7;
        is_bright = (br >= 140);
    } else {
        if (which == 0) col = 1;      // red
        else if (which == 1) col = 2; // green
        else col = 4;                 // blue

        // yellow/magenta/cyan for mixes
        if (which == 0 && g > 120 && b < 100) col = 3; // yellow
        if (which == 0 && b > 120 && g < 100) col = 5; // magenta
        if (which == 1 && b > 120 && r < 100) col = 6; // cyan
        if (which == 2 && g > 120 && r < 100) col = 6; // cyan-ish
        if (which == 1 && r > 120 && b < 100) col = 3; // yellow-ish
        if (which == 2 && r > 120 && g < 100) col = 5; // magenta-ish
    }

    if (fg) *fg = col;
    if (bright) *bright = is_bright ? 1 : 0;
}

static void render_ansi_line_with_matches(const char *raw,
                                         const int *matched_mask, int matched_len,
                                         int y, int x_start, int max_x,
                                         attr_t base_attr, short base_pair) {
    if (!raw) return;

    int x = x_start;
    int plain_pos = 0;

    AnsiStyle st;
    st.bold = 0;
    st.underline = 0;
    st.reverse = 0;
    st.fg = -1;
    st.bright = 0;

    ansi_apply_style(&st, base_attr, base_pair);

    for (int i = 0; raw[i] && x < max_x; ) {
        unsigned char c = (unsigned char)raw[i];

        if (c == 0x1B) {
            unsigned char n = (unsigned char)raw[i + 1];
            if (!n) { i++; continue; }

            // OSC
            if (n == ']') {
                i += 2;
                while (raw[i]) {
                    if (raw[i] == '\a') { i++; break; }
                    if (raw[i] == 0x1B && raw[i + 1] == '\\') { i += 2; break; }
                    i++;
                }
                continue;
            }

            // CSI
            if (n == '[') {
                i += 2;

                int params[64];
                int pc = 0;
                int val = -1;

                // parse until command byte (0x40..0x7E)
                while (raw[i]) {
                    unsigned char b = (unsigned char)raw[i];
                    if (b >= 0x40 && b <= 0x7E) break;

                    if (isdigit((unsigned char)raw[i])) {
                        if (val < 0) val = 0;
                        val = val * 10 + (raw[i] - '0');
                    } else if (raw[i] == ';') {
                        if (pc < (int)(sizeof(params)/sizeof(params[0])))
                            params[pc++] = (val < 0 ? 0 : val);
                        val = -1;
                    }
                    i++;
                }

                unsigned char cmd = (unsigned char)raw[i];
                if (cmd) i++; // consume cmd

                if (cmd == 'm') {
                    if (pc < (int)(sizeof(params)/sizeof(params[0])))
                        params[pc++] = (val < 0 ? 0 : val);

                    for (int k = 0; k < pc; k++) {
                        int p = params[k];

                        if (p == 0) { st.bold = st.underline = st.reverse = 0; st.fg = -1; st.bright = 0; }
                        else if (p == 1) st.bold = 1;
                        else if (p == 22) st.bold = 0;
                        else if (p == 4) st.underline = 1;
                        else if (p == 24) st.underline = 0;
                        else if (p == 7) st.reverse = 1;
                        else if (p == 27) st.reverse = 0;
                        else if (p == 39) { st.fg = -1; st.bright = 0; }
                        else if (p >= 30 && p <= 37) { st.fg = p - 30; st.bright = 0; }
                        else if (p >= 90 && p <= 97) { st.fg = p - 90; st.bright = 1; }

                        // 256-color / truecolor fg: 38;5;N or 38;2;R;G;B
                        else if (p == 38) {
                            if (k + 1 < pc && params[k + 1] == 5) {
                                if (k + 2 < pc) {
                                    int nidx = params[k + 2];
                                    int rr, gg, bb;
                                    xterm256_to_rgb(nidx, &rr, &gg, &bb);
                                    rgb_to_ansi16(rr, gg, bb, &st.fg, &st.bright);
                                    k += 2;
                                }
                            } else if (k + 1 < pc && params[k + 1] == 2) {
                                if (k + 4 < pc) {
                                    int rr = params[k + 2];
                                    int gg = params[k + 3];
                                    int bb = params[k + 4];
                                    if (rr < 0) rr = 0; if (rr > 255) rr = 255;
                                    if (gg < 0) gg = 0; if (gg > 255) gg = 255;
                                    if (bb < 0) bb = 0; if (bb > 255) bb = 255;
                                    rgb_to_ansi16(rr, gg, bb, &st.fg, &st.bright);
                                    k += 4;
                                }
                            }
                        }

                        // background could be 48;... but we ignore it (ncurses bg mapping is messy)
                    }

                    ansi_apply_style(&st, base_attr, base_pair);
                }

                continue;
            }

            // Any other ESC sequence: skip ESC + next byte to avoid junk
            i += 2;
            continue;
        }

        if (c == '\t') c = ' ';

        int is_match = 0;
        if (matched_mask && plain_pos >= 0 && plain_pos < matched_len && matched_mask[plain_pos]) {
            is_match = 1;
        }

        if (is_match) attron(A_REVERSE | A_BOLD);
        mvaddch(y, x++, (chtype)c);
        if (is_match) {
            attroff(A_REVERSE | A_BOLD);
            ansi_apply_style(&st, base_attr, base_pair);
        }

        plain_pos++;
        i++;
    }

    attrset(base_attr | COLOR_PAIR(base_pair));
}

static void refresh_live_command(FuzzyState *st) {
    if (!st->live_mode || !st->live_cmd || !st->live_cmd[0]) return;

    char want[MAX_LINE_LEN];
    want[0] = '\0';
    if (st->match_count > 0 && st->selected >= 0 && st->selected < st->match_count) {
        int old_idx = st->match_indices[st->selected];
        const char *s = st->lines[old_idx];
        if (s) {
            strncpy(want, s, sizeof(want) - 1);
            want[sizeof(want) - 1] = '\0';
        }
    }

    int old_line_count = st->line_count;
    char **old_lines = NULL;
    char **old_raw_lines = NULL;

    if (old_line_count > 0) {
        old_lines = (char**)calloc((size_t)old_line_count, sizeof(char*));
        old_raw_lines = (char**)calloc((size_t)old_line_count, sizeof(char*));
        if (old_lines && old_raw_lines) {
            for (int i = 0; i < old_line_count; i++) {
                old_lines[i] = st->lines[i];
                old_raw_lines[i] = st->raw_lines[i];
            }
        } else {
            free(old_lines);
            free(old_raw_lines);
            old_lines = NULL;
            old_raw_lines = NULL;
        }
    }

    st->line_count = 0;

    FILE *fp = popen(st->live_cmd, "r");
    if (!fp) {
        if (old_lines && old_raw_lines) {
            for (int i = 0; i < old_line_count; i++) {
                st->lines[i] = old_lines[i];
                st->raw_lines[i] = old_raw_lines[i];
            }
            st->line_count = old_line_count;
            free(old_lines);
            free(old_raw_lines);
        }
        return;
    }

    load_stream(st, fp);
    pclose(fp);

    int success = (st->line_count > 0);

    if (!success) {
        for (int i = 0; i < st->line_count; i++) {
            free(st->lines[i]);
            free(st->raw_lines[i]);
            st->lines[i] = NULL;
            st->raw_lines[i] = NULL;
        }
        st->line_count = 0;

        if (old_lines && old_raw_lines) {
            for (int i = 0; i < old_line_count; i++) {
                st->lines[i] = old_lines[i];
                st->raw_lines[i] = old_raw_lines[i];
            }
            st->line_count = old_line_count;
            free(old_lines);
            free(old_raw_lines);
        }
        return;
    }

    if (old_lines && old_raw_lines) {
        for (int i = 0; i < old_line_count; i++) {
            free(old_lines[i]);
            free(old_raw_lines[i]);
        }
        free(old_lines);
        free(old_raw_lines);
    }

    update_matches(st);

    if (want[0] != '\0' && st->match_count > 0) {
        for (int m = 0; m < st->match_count; m++) {
            int idx = st->match_indices[m];
            if (st->lines[idx] && strcmp(st->lines[idx], want) == 0) {
                st->selected = m;
                ensure_visible(st);
                return;
            }
        }
    }

    st->selected = 0;
    st->scroll_offset = 0;
    clear();
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
                    int bonus = 5 * consecutive;
                    if (score > INT_MAX - bonus) score = INT_MAX;
                    else score += bonus;
                }
                consecutive++;

                if (h_idx == 0 ||
                    haystack[h_idx - 1] == ' ' ||
                    haystack[h_idx - 1] == '/' ||
                    haystack[h_idx - 1] == '_') {
                    if (score <= INT_MAX - 10) score += 10;
                    else score = INT_MAX;
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

static int regex_score(const char *pattern, const char *haystack, int case_sensitive,
                       regex_t *regex, int *regex_valid, char *regex_error, size_t error_size) {
    if (!pattern || !*pattern) {
        *regex_valid = 0;
        return 1000;
    }

    if (!*regex_valid) {
        int flags = REG_EXTENDED | REG_NOSUB;
        if (!case_sensitive) flags |= REG_ICASE;

        int ret = regcomp(regex, pattern, flags);
        if (ret != 0) {
            regerror(ret, regex, regex_error, error_size);
            *regex_valid = -1;
            return -1;
        }
        *regex_valid = 1;
    }

    if (*regex_valid < 0) return -1;

    int ret = regexec(regex, haystack, 0, NULL, 0);
    if (ret == 0) return 1000;
    return -1;
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
static int compare_scores_bsd(void *state, const void *a, const void *b) {
    return compare_scores(a, b, state);
}
#elif defined(__linux__) || defined(_GNU_SOURCE)
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
        int score = -1;

        switch (st->match_mode) {
            case MATCH_EXACT: {
                const char *found = st->case_sensitive ?
                    strstr(st->lines[i], st->query) :
                    strcasestr(st->lines[i], st->query);
                score = found ? 1000 : -1;
                break;
            }

            case MATCH_REGEX:
                score = regex_score(st->query, st->lines[i], st->case_sensitive,
                                    &st->regex, &st->regex_valid,
                                    st->regex_error, sizeof(st->regex_error));
                break;

            case MATCH_FUZZY:
            default:
                score = fuzzy_score(st->query, st->lines[i], st->case_sensitive);
                break;
        }

        st->scores[i] = score;
        if (score >= 0) st->match_indices[st->match_count++] = i;
    }

#if defined(__APPLE__) || defined(__FreeBSD__)
    qsort_r(st->match_indices, (size_t)st->match_count, sizeof(int), st, compare_scores_bsd);
#elif defined(__linux__) || defined(_GNU_SOURCE)
    qsort_r(st->match_indices, (size_t)st->match_count, sizeof(int), compare_scores_gnu, st);
#else
    g_sort_ctx = st;
    qsort(st->match_indices, (size_t)st->match_count, sizeof(int), compare_scores_static);
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

    char *raw = strdup(s);
    if (!raw) {
        fprintf(stderr, "Warning: failed to allocate memory for raw line\n");
        return;
    }

    char clean[MAX_LINE_LEN];
    strip_ansi(s, clean, sizeof(clean));

    char *plain = strdup(clean);
    if (!plain) {
        fprintf(stderr, "Warning: failed to allocate memory for line\n");
        free(raw);
        return;
    }

    if (!st->ansi_render && has_ansi_escape(s)) {
        st->ansi_render = 1;
    }

    st->raw_lines[st->line_count] = raw;
    st->lines[st->line_count] = plain;
    st->line_count++;
}

static void add_line_grep(FuzzyState *st, const char *filename, int line_num, const char *content) {
    if (st->line_count >= MAX_LINES) return;
    if (!content || !*content) return;

    char formatted[MAX_LINE_LEN];
    snprintf(formatted, sizeof(formatted), "%s:%d:%s", filename, line_num, content);

    add_line(st, formatted);

    if (st->line_numbers) {
        st->line_numbers[st->line_count - 1] = line_num;
    }
}

static void load_stream(FuzzyState *st, FILE *fp) {
    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), fp) && st->line_count < MAX_LINES) {
        size_t len = strlen(line);

        if (len > 0 && len == MAX_LINE_LEN - 1 && line[len - 1] != '\n') {
            int ch;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {}
            fprintf(stderr, "Warning: line truncated (exceeds %d bytes)\n", MAX_LINE_LEN);
        }

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        add_line(st, line);
    }
}

static void load_file_grep(FuzzyState *st, const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Warning: failed to open '%s': %s\n", filename, strerror(errno));
        return;
    }

    char line[MAX_LINE_LEN];
    int line_num = 1;

    while (fgets(line, sizeof(line), fp) && st->line_count < MAX_LINES) {
        size_t len = strlen(line);

        if (len > 0 && len == MAX_LINE_LEN - 1 && line[len - 1] != '\n') {
            int ch;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {}
        }

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (len > 0) {
            add_line_grep(st, filename, line_num, line);
        }

        line_num++;
    }

    fclose(fp);
}

static void clear_lines(FuzzyState *st) {
    for (int i = 0; i < st->line_count; i++) {
        free(st->lines[i]);
        free(st->raw_lines[i]);
        st->lines[i] = NULL;
        st->raw_lines[i] = NULL;
    }
    st->line_count = 0;
}

static void load_directory(FuzzyState *st, const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "Failed to open directory: %s\n", path);
        return;
    }

    clear_lines(st);

    struct dirent *entry;
    int skipped = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (st->line_count >= MAX_LINES) {
            skipped++;
            continue;
        }

        if (strcmp(entry->d_name, ".") == 0) continue;

        int is_parent = (strcmp(entry->d_name, "..") == 0);

        if (!st->show_hidden && !is_parent && entry->d_name[0] == '.') {
            continue;
        }

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        struct stat st_buf;

        if (is_parent) {
            add_line(st, "../");
        } else if (stat(full_path, &st_buf) == 0) {
            if (S_ISDIR(st_buf.st_mode)) {
                char dir_entry[MAX_LINE_LEN];
                snprintf(dir_entry, sizeof(dir_entry), "%s/", entry->d_name);
                add_line(st, dir_entry);
            } else {
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

static char *sh_sq(const char *in) {
    if (!in) return NULL;

    size_t len = strlen(in);
    size_t extra = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '\'') extra += 3;
    }

    char *out = (char*)malloc(len + extra + 3);
    if (!out) return NULL;

    char *p = out;
    *p++ = '\'';
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '\'') {
            memcpy(p, "'\\''", 4);
            p += 4;
        } else {
            *p++ = in[i];
        }
    }
    *p++ = '\'';
    *p = '\0';
    return out;
}

static int parse_ssh_path(const char *path, char *user, size_t user_size,
                          char *host, size_t host_size,
                          char *remote_path, size_t remote_path_size) {
    if (!path || !user || !host || !remote_path) return 0;

    const char *colon = strchr(path, ':');
    if (!colon) return 0;

    size_t userhost_len = (size_t)(colon - path);
    if (userhost_len >= 1024) return 0;

    char temp[1024];
    memcpy(temp, path, userhost_len);
    temp[userhost_len] = '\0';

    const char *rpath = colon + 1;

    const char *at = strchr(temp, '@');
    if (at) {
        size_t user_len = (size_t)(at - temp);
        size_t host_len = userhost_len - user_len - 1;

        if (user_len >= user_size || host_len >= host_size) return 0;

        memcpy(user, temp, user_len);
        user[user_len] = '\0';

        memcpy(host, at + 1, host_len);
        host[host_len] = '\0';
    } else {
        if (userhost_len >= host_size) return 0;

        user[0] = '\0';
        memcpy(host, temp, userhost_len);
        host[userhost_len] = '\0';
    }

    size_t rpath_len = strlen(rpath);
    if (rpath_len >= remote_path_size) return 0;

    memcpy(remote_path, rpath, rpath_len);
    remote_path[rpath_len] = '\0';

    return 1;
}

static FILE *ssh_popen(const char *user, const char *host, const char *command) {
    if (!host || !host[0] || !command || !command[0]) {
        return NULL;
    }

    char *qcmd = sh_sq(command);
    if (!qcmd) return NULL;

    char ssh_cmd[4096];
    int written;

    if (user && user[0]) {
        written = snprintf(ssh_cmd, sizeof(ssh_cmd),
                           "ssh -o ConnectTimeout=10 -o BatchMode=yes %s@%s %s 2>&1",
                           user, host, qcmd);
    } else {
        written = snprintf(ssh_cmd, sizeof(ssh_cmd),
                           "ssh -o ConnectTimeout=10 -o BatchMode=yes %s %s 2>&1",
                           host, qcmd);
    }

    free(qcmd);

    if (written < 0 || written >= (int)sizeof(ssh_cmd)) {
        fprintf(stderr, "SSH command too long\n");
        return NULL;
    }

    return popen(ssh_cmd, "r");
}

static void load_ssh_directory(FuzzyState *st, const char *path) {
    if (!path || !path[0]) {
        fprintf(stderr, "Invalid remote path\n");
        return;
    }

    clear_lines(st);

    char *qpath = sh_sq(path);
    if (!qpath) {
        fprintf(stderr, "Out of memory\n");
        return;
    }

    char ls_cmd[2048];
    int written;

    if (st->show_hidden) {
        written = snprintf(ls_cmd, sizeof(ls_cmd),
                           "cd -- %s 2>/dev/null && ls -Ap1 --color=never 2>/dev/null || ls -Ap1 2>/dev/null",
                           qpath);
    } else {
        written = snprintf(ls_cmd, sizeof(ls_cmd),
                           "cd -- %s 2>/dev/null && ls -p1 --color=never 2>/dev/null || ls -p1 2>/dev/null",
                           qpath);
    }

    free(qpath);

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

        if (strcmp(line, ".") == 0) continue;

        if (strncmp(line, "Permission denied", 17) == 0 ||
            strncmp(line, "Connection refused", 18) == 0 ||
            strncmp(line, "Host key verification failed", 28) == 0) {
            fprintf(stderr, "SSH error: %s\n", line);
            pclose(fp);
            return;
        }

        int is_dir = (len > 0 && line[len - 1] == '/');

        if (!is_dir && strcmp(line, "..") != 0) {
            char name[MAX_LINE_LEN];
            strncpy(name, line, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';

            size_t nlen = strlen(name);
            if (nlen > 0 && name[nlen - 1] == '*') name[nlen - 1] = '\0';

            char full[MAX_LINE_LEN * 2];
            snprintf(full, sizeof(full), "%s/%s", path, name);

            char *qfull = sh_sq(full);
            if (qfull) {
                char stat_cmd[2048];
                int stat_written = snprintf(stat_cmd, sizeof(stat_cmd),
                                            "[ -x %s ] && echo x || echo n", qfull);
                free(qfull);

                if (stat_written > 0 && stat_written < (int)sizeof(stat_cmd)) {
                    FILE *stat_fp = ssh_popen(st->ssh_user, st->ssh_host, stat_cmd);
                    if (stat_fp) {
                        char result[8] = {0};
                        if (fgets(result, sizeof(result), stat_fp)) {
                            if (result[0] == 'x') {
                                len = strlen(line);
                                if (len < MAX_LINE_LEN - 2) {
                                    line[len] = '*';
                                    line[len + 1] = '\0';
                                }
                            }
                        }
                        pclose(stat_fp);
                    }
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

static int load_ssh_file(FuzzyState *st, const char *path) {
    if (!path || !path[0]) return 0;

    char user[256], host[256], remote_path[256];

    if (!parse_ssh_path(path, user, sizeof(user), host, sizeof(host), remote_path, sizeof(remote_path))) {
        return 0;
    }

    strncpy(st->ssh_host, host, sizeof(st->ssh_host) - 1);
    st->ssh_host[sizeof(st->ssh_host) - 1] = '\0';
    strncpy(st->ssh_user, user, sizeof(st->ssh_user) - 1);
    st->ssh_user[sizeof(st->ssh_user) - 1] = '\0';

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

static int load_files_grep(FuzzyState *st, int argc, char **argv, int first_file_idx) {
    int loaded_any = 0;

    for (int i = first_file_idx; i < argc; i++) {
        const char *path = argv[i];

        if (strchr(path, ':')) {
            fprintf(stderr, "Warning: SSH paths not supported in grep mode: %s\n", path);
            continue;
        }

        load_file_grep(st, path);
        loaded_any = 1;

        if (st->line_count >= MAX_LINES) {
            fprintf(stderr, "Warning: MAX_LINES (%d) reached, some files not loaded\n", MAX_LINES);
            break;
        }
    }

    return loaded_any;
}

static void load_stdin(FuzzyState *st) {
    load_stream(st, stdin);
}

static void free_state(FuzzyState *st) {
    for (int i = 0; i < st->line_count; i++) {
        free(st->lines[i]);
        free(st->raw_lines[i]);
        st->lines[i] = NULL;
        st->raw_lines[i] = NULL;
    }

    free(st->scores);
    free(st->match_indices);
    free(st->source_files);
    free(st->line_numbers);

    if (st->input_files) {
        for (int i = 0; i < st->input_file_count; i++) {
            free(st->input_files[i]);
        }
        free(st->input_files);
    }

    if (st->regex_valid > 0) {
        regfree(&st->regex);
    }

    free(st->live_cmd);
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

    mvprintw(max_y - 1, 1, "NBL Fuzzy Filter | ");
    int nbl_len = 19;

    const char *mode_str;
    int mode_color;
    if (st->mode == MODE_INSERT) {
        mode_str = "INSERT";
        mode_color = COLOR_QUERY;
    } else {
        mode_str = "NORMAL";
        mode_color = COLOR_MATCH;
    }

    attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    attron(COLOR_PAIR(mode_color) | A_BOLD);
    mvprintw(max_y - 1, nbl_len, " [%s]", mode_str);
    attroff(COLOR_PAIR(mode_color) | A_BOLD);
    attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);

    int mode_len = (int)strlen(mode_str) + 3;
    int status_start = nbl_len + mode_len;

    const char *match_mode_str;
    switch (st->match_mode) {
        case MATCH_EXACT: match_mode_str = "EXACT"; break;
        case MATCH_REGEX: match_mode_str = "REGEX"; break;
        default: match_mode_str = "FUZZY"; break;
    }

    char left[256];
    snprintf(left, sizeof(left), " | %d/%d matches | Mode: %s%s%s%s%s%s",
             st->match_count > 0 ? st->selected + 1 : 0,
             st->match_count,
             match_mode_str,
             st->case_sensitive ? " (case)" : "",
             st->show_hidden ? " | hidden" : "",
             st->is_directory_mode ? " | dir" : "",
             st->ssh_mode ? " | SSH" : "",
             st->grep_mode ? " | grep" : "");

    mvprintw(max_y - 1, status_start, "%s", left);

    if (st->match_mode == MATCH_REGEX && st->regex_valid < 0) {
        attron(COLOR_PAIR(COLOR_ERROR) | A_BOLD);
        int error_x = status_start + (int)strlen(left) + 2;
        if (error_x < max_x - 20) {
            mvprintw(max_y - 1, error_x, "| REGEX ERROR");
        }
        attroff(COLOR_PAIR(COLOR_ERROR) | A_BOLD);
        attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    }

    if (st->query_len > 0) {
        char right[300];
        snprintf(right, sizeof(right), "Query: %s ", st->query);
        int rx = max_x - (int)strlen(right) - 1;
        if (rx < status_start) rx = status_start;
        mvprintw(max_y - 1, rx, "%s", right);
    } else if (st->is_directory_mode) {
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

static void highlight_matches_plain(const char *line, const char *query, int y, int x_start, int max_x, int case_sensitive) {
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
            mvaddch(y, x++, (chtype)line[i]);
            attroff(COLOR_PAIR(COLOR_MATCH) | A_BOLD);
        } else {
            mvaddch(y, x++, (chtype)line[i]);
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
        const char *plain = st->lines[line_idx];
        const char *raw   = st->raw_lines[line_idx];

        int is_selected = (match_idx == st->selected);
        int is_executable = (plain && strlen(plain) > 0 && plain[strlen(plain) - 1] == '*');

        attr_t base_attr = 0;
        short base_pair = COLOR_NORMAL;

        if (is_selected) {
            base_attr = A_REVERSE | A_BOLD;
            base_pair = COLOR_SELECTED;
            attron(COLOR_PAIR(COLOR_SELECTED) | A_REVERSE | A_BOLD);
            mvhline(i, 0, ' ', max_x);
        } else if (is_executable) {
            base_attr = 0;
            base_pair = COLOR_EXECUTABLE;
            attron(COLOR_PAIR(COLOR_EXECUTABLE));
        } else {
            base_attr = 0;
            base_pair = COLOR_NORMAL;
            attron(COLOR_PAIR(COLOR_NORMAL));
        }

        mvprintw(i, 1, is_selected ? "> " : "  ");

        if (st->ansi_render && raw) {
            int mask[MAX_LINE_LEN];
            build_matched_mask(plain ? plain : "", st->query, st->case_sensitive, mask, MAX_LINE_LEN);
            render_ansi_line_with_matches(raw, mask, MAX_LINE_LEN, i, 3, max_x, base_attr, base_pair);
        } else {
            highlight_matches_plain(plain ? plain : "", st->query, i, 3, max_x, st->case_sensitive);
        }

        if (is_selected) {
            attroff(COLOR_PAIR(COLOR_SELECTED) | A_REVERSE | A_BOLD);
        } else if (is_executable) {
            attroff(COLOR_PAIR(COLOR_EXECUTABLE));
        } else {
            attroff(COLOR_PAIR(COLOR_NORMAL));
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

        if (st->match_mode == MATCH_REGEX && st->regex_valid > 0) {
            regfree(&st->regex);
            st->regex_valid = 0;
        }

        update_matches(st);
    }
}

static void delete_char(FuzzyState *st) {
    if (st->query_len > 0) {
        st->query[--st->query_len] = '\0';

        if (st->match_mode == MATCH_REGEX && st->regex_valid > 0) {
            regfree(&st->regex);
            st->regex_valid = 0;
        }

        update_matches(st);
    }
}

static void delete_word(FuzzyState *st) {
    while (st->query_len > 0) {
        char c = st->query[st->query_len - 1];
        st->query[--st->query_len] = '\0';
        if (c == ' ' || c == '/' || c == '_' || c == '-') break;
    }

    if (st->match_mode == MATCH_REGEX && st->regex_valid > 0) {
        regfree(&st->regex);
        st->regex_valid = 0;
    }

    update_matches(st);
}

static void clear_query(FuzzyState *st) {
    st->query[0] = '\0';
    st->query_len = 0;

    if (st->match_mode == MATCH_REGEX && st->regex_valid > 0) {
        regfree(&st->regex);
        st->regex_valid = 0;
    }

    update_matches(st);
}

static void toggle_exact_mode(FuzzyState *st) {
    if (st->match_mode == MATCH_EXACT) st->match_mode = MATCH_FUZZY;
    else st->match_mode = MATCH_EXACT;

    if (st->regex_valid > 0) {
        regfree(&st->regex);
        st->regex_valid = 0;
    }
    update_matches(st);
}

static void toggle_fuzzy_mode(FuzzyState *st) {
    if (st->match_mode == MATCH_FUZZY) st->match_mode = MATCH_EXACT;
    else st->match_mode = MATCH_FUZZY;

    if (st->regex_valid > 0) {
        regfree(&st->regex);
        st->regex_valid = 0;
    }
    update_matches(st);
}

static void toggle_regex_mode(FuzzyState *st) {
    if (st->match_mode == MATCH_REGEX) {
        if (st->regex_valid > 0) {
            regfree(&st->regex);
            st->regex_valid = 0;
        }
        st->match_mode = MATCH_FUZZY;
    } else {
        if (st->regex_valid > 0) {
            regfree(&st->regex);
        }
        st->match_mode = MATCH_REGEX;
        st->regex_valid = 0;
    }
    update_matches(st);
}

static void store_input_files(FuzzyState *st, int argc, char **argv, int first_file_idx) {
    if (first_file_idx >= argc) {
        st->input_file_count = 0;
        st->input_files = NULL;
        return;
    }

    int count = argc - first_file_idx;
    st->input_files = (char**)calloc((size_t)count, sizeof(char*));
    if (!st->input_files) {
        fprintf(stderr, "Warning: failed to allocate memory for file list\n");
        st->input_file_count = 0;
        return;
    }

    st->input_file_count = 0;
    for (int i = first_file_idx; i < argc; i++) {
        st->input_files[st->input_file_count] = strdup(argv[i]);
        if (st->input_files[st->input_file_count]) {
            st->input_file_count++;
        }
    }
}

static void refresh_source(FuzzyState *st) {
    if (st->live_mode) {
        refresh_live_command(st);
        return;
    }

    int old_line_count = st->line_count;
    char **old_lines = NULL;
    char **old_raw_lines = NULL;

    if (st->line_count > 0) {
        old_lines = (char**)calloc((size_t)st->line_count, sizeof(char*));
        old_raw_lines = (char**)calloc((size_t)st->line_count, sizeof(char*));
        if (old_lines && old_raw_lines) {
            for (int i = 0; i < st->line_count; i++) {
                old_lines[i] = st->lines[i];
                old_raw_lines[i] = st->raw_lines[i];
            }
        } else {
            free(old_lines);
            free(old_raw_lines);
            old_lines = NULL;
            old_raw_lines = NULL;
        }
    }

    st->line_count = 0;

    int success = 0;

    if (st->is_directory_mode) {
        if (st->ssh_mode) load_ssh_directory(st, st->current_dir);
        else load_directory(st, st->current_dir);
        success = (st->line_count > 0);

    } else if (st->from_stdin) {
        if (old_lines && old_raw_lines) {
            for (int i = 0; i < old_line_count; i++) {
                st->lines[i] = old_lines[i];
                st->raw_lines[i] = old_raw_lines[i];
            }
            st->line_count = old_line_count;
            free(old_lines);
            free(old_raw_lines);
        }
        return;

    } else if (st->input_file_count > 0 && st->input_files) {
        for (int i = 0; i < st->input_file_count && st->line_count < MAX_LINES; i++) {
            const char *path = st->input_files[i];

            if (strchr(path, ':')) {
                if (load_ssh_file(st, path)) {
                    success = 1;
                    continue;
                }
            }

            if (st->grep_mode) {
                load_file_grep(st, path);
                success = 1;
            } else {
                FILE *fp = fopen(path, "r");
                if (fp) {
                    load_stream(st, fp);
                    fclose(fp);
                    success = 1;
                }
            }
        }
    } else {
        if (old_lines && old_raw_lines) {
            for (int i = 0; i < old_line_count; i++) {
                st->lines[i] = old_lines[i];
                st->raw_lines[i] = old_raw_lines[i];
            }
            st->line_count = old_line_count;
            free(old_lines);
            free(old_raw_lines);
        }
        return;
    }

    if (success && old_lines && old_raw_lines) {
        for (int i = 0; i < old_line_count; i++) {
            free(old_lines[i]);
            free(old_raw_lines[i]);
        }
        free(old_lines);
        free(old_raw_lines);
    } else if (!success && old_lines && old_raw_lines) {
        for (int i = 0; i < st->line_count; i++) {
            free(st->lines[i]);
            free(st->raw_lines[i]);
            st->lines[i] = NULL;
            st->raw_lines[i] = NULL;
        }
        for (int i = 0; i < old_line_count; i++) {
            st->lines[i] = old_lines[i];
            st->raw_lines[i] = old_raw_lines[i];
        }
        st->line_count = old_line_count;
        free(old_lines);
        free(old_raw_lines);
        return;
    }

    update_matches(st);
    st->selected = 0;
    st->scroll_offset = 0;
    clear();
}

static void toggle_hidden_files(FuzzyState *st) {
    if (!st->is_directory_mode) return;

    st->show_hidden = !st->show_hidden;

    if (st->ssh_mode) load_ssh_directory(st, st->current_dir);
    else load_directory(st, st->current_dir);

    update_matches(st);

    st->selected = 0;
    st->scroll_offset = 0;

    clear();
}

static void navigate_directory(FuzzyState *st, const char *selection) {
    if (!st->is_directory_mode) return;
    if (!selection || !*selection) return;

    char new_path[PATH_MAX];

    if (strcmp(selection, "../") == 0 || strcmp(selection, "..") == 0) {
        char *last_slash = strrchr(st->current_dir, '/');
        if (last_slash && last_slash != st->current_dir) {
            *last_slash = '\0';
            strncpy(new_path, st->current_dir, PATH_MAX - 1);
            *last_slash = '/';
        } else {
            strncpy(new_path, "/", PATH_MAX - 1);
        }
    } else if (selection[strlen(selection) - 1] == '/') {
        char dirname[MAX_LINE_LEN];
        strncpy(dirname, selection, sizeof(dirname) - 1);
        dirname[sizeof(dirname) - 1] = '\0';

        size_t len = strlen(dirname);
        if (len > 0 && dirname[len - 1] == '/') dirname[len - 1] = '\0';

        if (strcmp(st->current_dir, "/") == 0) {
            snprintf(new_path, sizeof(new_path), "/%s", dirname);
        } else {
            snprintf(new_path, sizeof(new_path), "%s/%s", st->current_dir, dirname);
        }
    } else {
        return;
    }

    strncpy(st->current_dir, new_path, PATH_MAX - 1);
    st->current_dir[PATH_MAX - 1] = '\0';

    st->query[0] = '\0';
    st->query_len = 0;

    if (st->ssh_mode) load_ssh_directory(st, st->current_dir);
    else load_directory(st, st->current_dir);

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
            case 3:
                *running = 0;
                return -1;

            case 27:
                *running = 0;
                return -1;

            case '\n':
            case '\r':
            case KEY_ENTER:
                if (st->is_directory_mode && st->match_count > 0 && st->selected < st->match_count) {
                    int line_idx = st->match_indices[st->selected];
                    const char *selection = st->lines[line_idx];

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

            case 4:
                page_down(st);
                break;

            case 21:
                page_up(st);
                break;

            case 12:
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
                if (st->is_directory_mode) navigate_directory(st, "..");
                break;

            case 5:
                toggle_exact_mode(st);
                break;

            case 6:
                toggle_fuzzy_mode(st);
                break;

            case 24:
                toggle_regex_mode(st);
                break;

            case 18:
                refresh_source(st);
                break;

            default:
                break;
        }
    } else {
        switch (ch) {
            case 27:
                st->mode = MODE_NORMAL;
                break;

            case '\n':
            case '\r':
            case KEY_ENTER:
                if (st->is_directory_mode && st->match_count > 0 && st->selected < st->match_count) {
                    int line_idx = st->match_indices[st->selected];
                    const char *selection = st->lines[line_idx];

                    if (selection && strlen(selection) > 0 &&
                        (selection[strlen(selection) - 1] == '/' ||
                         strcmp(selection, "..") == 0)) {
                        navigate_directory(st, selection);
                        break;
                    }
                }
                *running = 0;
                return st->selected;

            case KEY_BACKSPACE:
            case 127:
            case 8:
                delete_char(st);
                break;

            case 23:
                delete_word(st);
                break;

            case 12:
                clear_query(st);
                break;

            case 5:
                toggle_exact_mode(st);
                break;

            case 6:
                toggle_fuzzy_mode(st);
                break;

            case 24:
                toggle_regex_mode(st);
                break;

            case 18:
                refresh_source(st);
                break;

            default:
                if (ch == '.' && st->is_directory_mode && st->query_len == 0) {
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
            st->match_mode = MATCH_EXACT;

        } else if (strcmp(argv[i], "-r") == 0) {
            st->match_mode = MATCH_REGEX;

        } else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 < argc) st->delimiter = argv[++i][0];
            else {
                fprintf(stderr, "Error: -d requires an argument\n");
                return -1;
            }

        } else if (strcmp(argv[i], "-G") == 0) {
            st->grep_mode = 1;

            if (!st->line_numbers) {
                st->line_numbers = (int*)calloc(MAX_LINES, sizeof(int));
                if (!st->line_numbers) {
                    fprintf(stderr, "Failed to allocate line numbers array\n");
                    return -1;
                }
            }

        } else if (strcmp(argv[i], "-D") == 0) {
            st->is_directory_mode = 1;

            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const char *dir_arg = argv[++i];

                char user[256], host[256], remote_path[256];
                if (parse_ssh_path(dir_arg,
                                   user, sizeof(user),
                                   host, sizeof(host),
                                   remote_path, sizeof(remote_path))) {
                    st->ssh_mode = 1;

                    strncpy(st->ssh_user, user, sizeof(st->ssh_user) - 1);
                    st->ssh_user[sizeof(st->ssh_user) - 1] = '\0';

                    strncpy(st->ssh_host, host, sizeof(st->ssh_host) - 1);
                    st->ssh_host[sizeof(st->ssh_host) - 1] = '\0';

                    strncpy(st->current_dir, remote_path, PATH_MAX - 1);
                    st->current_dir[PATH_MAX - 1] = '\0';
                } else {
                    strncpy(st->current_dir, dir_arg, PATH_MAX - 1);
                    st->current_dir[PATH_MAX - 1] = '\0';
                }
            } else {
                if (!getcwd(st->current_dir, PATH_MAX)) {
                    fprintf(stderr, "Error: failed to get current directory: %s\n", strerror(errno));
                    return -1;
                }
            }

        } else if (strcmp(argv[i], "--live") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --live requires a command string\n");
                return -1;
            }

            free(st->live_cmd);
            st->live_cmd = strdup(argv[++i]);
            if (!st->live_cmd) {
                fprintf(stderr, "Error: out of memory\n");
                return -1;
            }
            st->live_mode = 1;

        } else if (strcmp(argv[i], "--interval") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --interval requires milliseconds\n");
                return -1;
            }

            int ms = atoi(argv[++i]);
            if (ms < 50) ms = 50;
            st->live_interval_ms = ms;

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
    char output[256];

    FuzzyState *st = (FuzzyState*)calloc(1, sizeof(FuzzyState));
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
    st->mode = MODE_NORMAL;
    st->is_directory_mode = 0;
    st->show_hidden = 0;
    st->ssh_mode = 0;
    st->current_dir[0] = '\0';
    st->match_mode = MATCH_FUZZY;
    st->regex_valid = 0;
    st->regex_error[0] = '\0';
    st->grep_mode = 0;
    st->line_numbers = NULL;
    st->source_files = NULL;
    st->input_files = NULL;
    st->input_file_count = 0;
    st->from_stdin = 0;
    st->live_mode = 0;
    st->live_cmd = NULL;
    st->live_interval_ms = 1000;
    st->last_live_refresh_ms = 0;
    st->ansi_render = 0;

    int first_file_idx = parse_flags(argc, argv, st);
    if (first_file_idx < 0) {
        free(st);
        return 0;
    }

    st->scores = (int*)calloc(MAX_LINES, sizeof(int));
    st->match_indices = (int*)calloc(MAX_LINES, sizeof(int));
    if (!st->scores || !st->match_indices) {
        fprintf(stderr, "Failed to allocate memory\n");
        free(st->scores);
        free(st->match_indices);
        free(st->line_numbers);
        free(st);
        return 1;
    }

    if (st->is_directory_mode) {
        if (st->ssh_mode) load_ssh_directory(st, st->current_dir);
        else load_directory(st, st->current_dir);

    } else if (st->live_mode) {
        refresh_live_command(st);

    } else if (!isatty(STDIN_FILENO)) {
        st->from_stdin = 1;
        load_stdin(st);

    } else {
        if (first_file_idx >= argc) {
            fprintf(stderr, "nfzf: no piped input, no files, and no --live command provided.\n\n");
            usage(argv[0]);
            free_state(st);
            free(st);
            return 1;
        }

        store_input_files(st, argc, argv, first_file_idx);

        int ok;
        if (st->grep_mode) ok = load_files_grep(st, argc, argv, first_file_idx);
        else ok = load_files(st, argc, argv, first_file_idx);

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

    if (st->live_mode) timeout(50);
    else timeout(-1);

    if (has_colors()) {
        start_color();
        use_default_colors();

        init_pair(COLOR_NORMAL,     COLOR_WHITE,   -1);
        init_pair(COLOR_SELECTED,   COLOR_WHITE,   -1);
        init_pair(COLOR_MATCH,      COLOR_YELLOW,  -1);
        init_pair(COLOR_STATUS,     COLOR_WHITE,   -1);
        init_pair(COLOR_QUERY,      COLOR_CYAN,    -1);
        init_pair(COLOR_EXECUTABLE, COLOR_GREEN,   -1);
        init_pair(COLOR_ERROR,      COLOR_RED,     -1);

        short map8[8] = {
            COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW,
            COLOR_BLUE,  COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE
        };

        for (int i = 0; i < 8; i++) {
            init_pair(ANSI_PAIR_BASE + i,     map8[i], -1);
            init_pair(ANSI_PAIR_BASE + 8 + i, map8[i], -1);
        }
    }

    int running = 1;
    int result = -1;

    while (running) {
        draw_ui(st);

        int r = handle_input(st, &running);
        if (!running) { result = r; break; }

        if (st->live_mode) {
            long t = now_ms();
            if (st->last_live_refresh_ms == 0) st->last_live_refresh_ms = t;

            if (t - st->last_live_refresh_ms >= st->live_interval_ms) {
                refresh_live_command(st);
                st->last_live_refresh_ms = t;
            }
        }
    }

    endwin();
    delscreen(scr);
    fclose(tty_in);
    fclose(tty_out);

    if (result >= 0 && result < st->match_count) {
        int line_idx = st->match_indices[result];
        const char *selected = st->lines[line_idx];

        strncpy(output, selected ? selected : "", sizeof(output) - 1);
        output[sizeof(output) - 1] = '\0';

        size_t len = strlen(output);
        if (len > 0 && (output[len - 1] == '/' || output[len - 1] == '*')) {
            output[len - 1] = '\0';
        }

        if (st->is_directory_mode) {
            if (strcmp(output, "..") == 0) {
                if (st->ssh_mode) {
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
                    const char *separator = (st->current_dir[strlen(st->current_dir) - 1] == '/') ? "" : "/";
                    if (st->ssh_user[0]) {
                        printf("%s@%s:%s%s%s\n", st->ssh_user, st->ssh_host, st->current_dir, separator, output);
                    } else {
                        printf("%s:%s%s%s\n", st->ssh_host, st->current_dir, separator, output);
                    }
                } else {
                    char full_path[PATH_MAX];
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