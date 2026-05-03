#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_TITLE "ctify"
#define WIN_W 1100
#define WIN_H 720
#define MAX_TRACKS 4096
#define TITLE_LEN 256
#define ARTIST_LEN 160
#define ALBUM_LEN 160
#define ROW_H 46
#define TOP_H 74
#define PLAYER_H 106
#define SIDEBAR_W 220
#define NOW_W 260
#define LEFT_PAD 28

typedef enum {
    VIEW_HOME = 0,
    VIEW_ALL = 1,
    VIEW_FAVORITES = 2
} ViewMode;

typedef struct {
    char path[PATH_MAX];
    char title[TITLE_LEN];
    char artist[ARTIST_LEN];
    char album[ALBUM_LEN];
    char folder[ARTIST_LEN];
    int duration_sec;
    bool favorite;
} Track;

typedef struct {
    Track tracks[MAX_TRACKS];
    int count;
    int selected;
    int scroll_y;
    int max_scroll;
    char root[PATH_MAX];
    char search[TITLE_LEN];
    bool search_active;
    bool ignore_next_text;
    ViewMode view;
} Library;

typedef struct {
    TTF_Font *sm;
    TTF_Font *md;
    TTF_Font *lg;
} UI;

typedef struct {
    pid_t pid;
    char socket_path[PATH_MAX];
    int current;
    bool playing;
    int volume;
    bool shuffle;
    bool repeat;
} Player;

static const char *font_paths[] = {
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    NULL
};

static int win_w = WIN_W;
static int win_h = WIN_H;

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    size_t len = strnlen(src, dst_len - 1);
    memmove(dst, src, len);
    dst[len] = '\0';
}

static bool path_join(char *out, size_t out_len, const char *left, const char *right)
{
    if (!out || out_len == 0) return false;
    if (!left) left = "";
    if (!right) right = "";

    int written;
    size_t left_len = strlen(left);
    if (left_len > 0 && left[left_len - 1] == '/')
        written = snprintf(out, out_len, "%s%s", left, right);
    else
        written = snprintf(out, out_len, "%s/%s", left, right);

    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static TTF_Font *load_font(int pt)
{
    for (int i = 0; font_paths[i]; i++) {
        TTF_Font *f = TTF_OpenFont(font_paths[i], pt);
        if (f) return f;
    }
    return NULL;
}

static void ui_close(UI *ui)
{
    if (!ui) return;
    if (ui->sm) TTF_CloseFont(ui->sm);
    if (ui->md) TTF_CloseFont(ui->md);
    if (ui->lg) TTF_CloseFont(ui->lg);
    ui->sm = ui->md = ui->lg = NULL;
}

static bool command_exists(const char *cmd)
{
    const char *path = getenv("PATH");
    if (!path) return false;

    char copy[4096];
    copy_text(copy, sizeof(copy), path);

    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), dir, cmd))
            continue;
        if (access(full, X_OK) == 0)
            return true;
    }
    return false;
}

static const char *basename_of(const char *path)
{
    const char *base = strrchr(path, '/');
    return (base && base[1]) ? base + 1 : path;
}

static const char *dirname_label(const char *path)
{
    static char buf[ARTIST_LEN];
    char copy[PATH_MAX];
    copy_text(copy, sizeof(copy), path);
    char *slash = strrchr(copy, '/');
    if (slash) *slash = '\0';
    copy_text(buf, sizeof(buf), basename_of(copy));
    return buf;
}

static void ensure_state_dir(char *out, size_t out_len)
{
    const char *state_home = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (state_home && state_home[0] != '\0')
        path_join(out, out_len, state_home, "ctify");
    else if (home && home[0] != '\0')
        snprintf(out, out_len, "%s/.local/state/ctify", home);
    else
        copy_text(out, out_len, ".ctify-state");
    mkdir(out, 0755);
}

static void favorites_file(char *out, size_t out_len)
{
    char state_dir[PATH_MAX];
    ensure_state_dir(state_dir, sizeof(state_dir));
    path_join(out, out_len, state_dir, "favorites.txt");
}

static void title_from_filename(const char *name, char *out, size_t out_len)
{
    copy_text(out, out_len, name);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    for (char *p = out; *p; p++) {
        if (*p == '_' || *p == '.') *p = ' ';
    }
}

static void trim_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
}

static void shell_quote(char *out, size_t out_len, const char *s)
{
    if (!out || out_len == 0) return;
    size_t pos = 0;
    out[pos++] = '\'';
    for (const char *p = s ? s : ""; *p && pos + 5 < out_len; p++) {
        if (*p == '\'') {
            memcpy(out + pos, "'\\''", 4);
            pos += 4;
        } else {
            out[pos++] = *p;
        }
    }
    if (pos + 1 < out_len) out[pos++] = '\'';
    out[pos] = '\0';
}

static void metadata_from_filename(Track *t)
{
    title_from_filename(basename_of(t->path), t->title, sizeof(t->title));
    copy_text(t->artist, sizeof(t->artist), t->folder);
    copy_text(t->album, sizeof(t->album), "");
    t->duration_sec = 0;
}

static void load_metadata(Track *t)
{
    metadata_from_filename(t);
    if (!command_exists("ffprobe")) return;

    char quoted[PATH_MAX + 64];
    shell_quote(quoted, sizeof(quoted), t->path);
    char cmd[PATH_MAX + 512];
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -show_entries format=duration:format_tags=title,artist,album -of default=nw=1:nk=0 %s",
             quoted);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        trim_line(line);
        if (strncmp(line, "TAG:title=", 10) == 0 && line[10])
            copy_text(t->title, sizeof(t->title), line + 10);
        else if (strncmp(line, "TAG:artist=", 11) == 0 && line[11])
            copy_text(t->artist, sizeof(t->artist), line + 11);
        else if (strncmp(line, "TAG:album=", 10) == 0 && line[10])
            copy_text(t->album, sizeof(t->album), line + 10);
        else if (strncmp(line, "duration=", 9) == 0)
            t->duration_sec = (int)(atof(line + 9) + 0.5);
    }
    pclose(fp);
}

static bool is_audio(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".mp3") == 0 ||
           strcasecmp(ext, ".flac") == 0 ||
           strcasecmp(ext, ".ogg") == 0 ||
           strcasecmp(ext, ".m4a") == 0 ||
           strcasecmp(ext, ".wav") == 0 ||
           strcasecmp(ext, ".aac") == 0 ||
           strcasecmp(ext, ".opus") == 0 ||
           strcasecmp(ext, ".wma") == 0;
}

static int cmp_track(const void *a, const void *b)
{
    const Track *ta = (const Track *)a;
    const Track *tb = (const Track *)b;
    int artist = strcasecmp(ta->artist, tb->artist);
    if (artist != 0) return artist;
    int album = strcasecmp(ta->album, tb->album);
    if (album != 0) return album;
    int title = strcasecmp(ta->title, tb->title);
    if (title != 0) return title;
    return strcasecmp(ta->path, tb->path);
}

static bool track_exists(const Library *lib, const char *path)
{
    for (int i = 0; i < lib->count; i++)
        if (strcmp(lib->tracks[i].path, path) == 0)
            return true;
    return false;
}

static void add_track(Library *lib, const char *path)
{
    if (lib->count >= MAX_TRACKS) return;
    if (track_exists(lib, path)) return;

    Track *t = &lib->tracks[lib->count++];
    copy_text(t->path, sizeof(t->path), path);
    copy_text(t->folder, sizeof(t->folder), dirname_label(path));
    t->favorite = false;
    load_metadata(t);
}

static void load_favorites(Library *lib)
{
    char path[PATH_MAX];
    favorites_file(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[PATH_MAX + 8];
    while (fgets(line, sizeof(line), fp)) {
        trim_line(line);
        if (line[0] == '\0') continue;
        for (int i = 0; i < lib->count; i++) {
            if (strcmp(lib->tracks[i].path, line) == 0) {
                lib->tracks[i].favorite = true;
                break;
            }
        }
    }
    fclose(fp);
}

static void save_favorites(const Library *lib)
{
    char path[PATH_MAX];
    favorites_file(path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    for (int i = 0; i < lib->count; i++)
        if (lib->tracks[i].favorite)
            fprintf(fp, "%s\n", lib->tracks[i].path);
    fclose(fp);
}

static void scan_dir(Library *lib, const char *dir_path, int depth)
{
    if (depth > 16) return;

    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), dir_path, entry->d_name))
            continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_dir(lib, full, depth + 1);
            continue;
        }
        if (S_ISREG(st.st_mode) && is_audio(entry->d_name))
            add_track(lib, full);
    }

    closedir(dir);
}

static void library_scan(Library *lib)
{
    lib->count = 0;
    lib->selected = -1;
    lib->scroll_y = 0;
    lib->max_scroll = 0;

    scan_dir(lib, lib->root, 0);
    qsort(lib->tracks, (size_t)lib->count, sizeof(Track), cmp_track);
    load_favorites(lib);
    lib->selected = lib->count > 0 ? 0 : -1;
}

static bool matches_search(const Library *lib, int idx)
{
    if (idx < 0 || idx >= lib->count) return false;
    const Track *t = &lib->tracks[idx];
    if (lib->view == VIEW_FAVORITES && !t->favorite) return false;
    if (lib->search[0] == '\0') return true;
    return strcasestr(t->title, lib->search) ||
           strcasestr(t->artist, lib->search) ||
           strcasestr(t->album, lib->search) ||
           strcasestr(t->folder, lib->search) ||
           strcasestr(t->path, lib->search);
}

static int visible_next(const Library *lib, int start, int dir)
{
    if (lib->count <= 0) return -1;
    int idx = start;
    for (int i = 0; i < lib->count; i++) {
        idx += dir;
        if (idx < 0) idx = lib->count - 1;
        if (idx >= lib->count) idx = 0;
        if (matches_search(lib, idx)) return idx;
    }
    return start >= 0 ? start : -1;
}

static void select_first_visible(Library *lib)
{
    if (lib->selected >= 0 && lib->selected < lib->count && matches_search(lib, lib->selected))
        return;
    lib->selected = visible_next(lib, -1, 1);
}

static int visible_count(const Library *lib)
{
    int count = 0;
    for (int i = 0; i < lib->count; i++)
        if (matches_search(lib, i))
            count++;
    return count;
}

static int visible_position(const Library *lib, int track_idx)
{
    int pos = 0;
    for (int i = 0; i < lib->count; i++) {
        if (!matches_search(lib, i)) continue;
        if (i == track_idx) return pos;
        pos++;
    }
    return -1;
}

static void ensure_selected_visible(Library *lib)
{
    int pos = visible_position(lib, lib->selected);
    if (pos < 0) return;
    int list_top = TOP_H;
    int list_h = win_h - TOP_H - PLAYER_H;
    int y = pos * ROW_H;
    if (y - lib->scroll_y < 0)
        lib->scroll_y = y;
    if (y + ROW_H - lib->scroll_y > list_h)
        lib->scroll_y = y + ROW_H - list_h;
    if (lib->scroll_y < 0) lib->scroll_y = 0;
    (void)list_top;
}

static int hit_test(const Library *lib, int mx, int my)
{
    int content_x = SIDEBAR_W;
    int content_w = win_w - SIDEBAR_W - NOW_W;
    if (mx < content_x || mx >= content_x + content_w) return -1;
    if (my < TOP_H || my >= win_h - PLAYER_H) return -1;

    int visible_idx = (my - TOP_H + lib->scroll_y) / ROW_H;
    int pos = 0;
    for (int i = 0; i < lib->count; i++) {
        if (!matches_search(lib, i)) continue;
        if (pos == visible_idx) return i;
        pos++;
    }
    return -1;
}

static int sidebar_hit(int mx, int my)
{
    if (mx < 0 || mx >= SIDEBAR_W || my < 0 || my >= win_h - PLAYER_H)
        return -1;
    if (my >= 94 && my < 132) return VIEW_HOME;
    if (my >= 138 && my < 176) return VIEW_ALL;
    if (my >= 228 && my < 266) return VIEW_FAVORITES;
    return -1;
}

static int ipc_send(const Player *p, const char *json)
{
    if (p->pid <= 0 || p->socket_path[0] == '\0') return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    copy_text(addr.sun_path, sizeof(addr.sun_path), p->socket_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    char msg[512];
    snprintf(msg, sizeof(msg), "%s\n", json);
    ssize_t len = (ssize_t)strlen(msg);
    ssize_t written = write(fd, msg, (size_t)len);
    close(fd);
    return written == len ? 0 : -1;
}

static void player_stop(Player *p)
{
    if (p->pid > 0) {
        ipc_send(p, "{\"command\":[\"quit\"]}");
        for (int i = 0; i < 20; i++) {
            int status = 0;
            pid_t done = waitpid(p->pid, &status, WNOHANG);
            if (done == p->pid) break;
            SDL_Delay(25);
        }
        kill(p->pid, SIGTERM);
        waitpid(p->pid, NULL, WNOHANG);
    }
    if (p->socket_path[0] != '\0')
        unlink(p->socket_path);
    p->pid = -1;
    p->socket_path[0] = '\0';
    p->current = -1;
    p->playing = false;
}

static void player_play(Player *p, const Library *lib, int idx)
{
    if (idx < 0 || idx >= lib->count) return;
    player_stop(p);

    snprintf(p->socket_path, sizeof(p->socket_path), "/tmp/ctify-%ld.sock", (long)getpid());
    unlink(p->socket_path);

    pid_t pid = fork();
    if (pid == 0) {
        char ipc_arg[PATH_MAX + 32];
        char volume_arg[64];
        snprintf(ipc_arg, sizeof(ipc_arg), "--input-ipc-server=%s", p->socket_path);
        snprintf(volume_arg, sizeof(volume_arg), "--volume=%d", p->volume);
        execlp("mpv", "mpv",
               "--no-video",
               "--really-quiet",
               "--force-window=no",
               ipc_arg,
               volume_arg,
               lib->tracks[idx].path,
               (char *)NULL);
        _exit(127);
    }

    if (pid > 0) {
        p->pid = pid;
        p->current = idx;
        p->playing = true;
    }
}

static void player_pause_toggle(Player *p)
{
    if (p->pid <= 0) return;
    ipc_send(p, "{\"command\":[\"cycle\",\"pause\"]}");
    p->playing = !p->playing;
}

static void player_set_volume(Player *p, int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    p->volume = volume;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "{\"command\":[\"set_property\",\"volume\",%d]}", p->volume);
    ipc_send(p, cmd);
}

static int random_visible(const Library *lib);

static void player_reap(Player *p, Library *lib)
{
    if (p->pid <= 0) return;
    int status = 0;
    pid_t done = waitpid(p->pid, &status, WNOHANG);
    if (done == p->pid) {
        int finished = p->current;
        p->pid = -1;
        p->playing = false;
        if (p->socket_path[0] != '\0') {
            unlink(p->socket_path);
            p->socket_path[0] = '\0';
        }
        if (p->repeat && finished >= 0) {
            player_play(p, lib, finished);
        } else {
            int next = p->shuffle ? random_visible(lib) : visible_next(lib, finished, 1);
            if (next >= 0 && next != finished) {
                lib->selected = next;
                ensure_selected_visible(lib);
                player_play(p, lib, next);
            }
        }
    }
}

static int random_visible(const Library *lib)
{
    int count = visible_count(lib);
    if (count <= 0) return -1;
    int target = rand() % count;
    int pos = 0;
    for (int i = 0; i < lib->count; i++) {
        if (!matches_search(lib, i)) continue;
        if (pos == target) return i;
        pos++;
    }
    return -1;
}

static void draw_rect(SDL_Renderer *r, int x, int y, int w, int h,
                      Uint8 rr, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_SetRenderDrawBlendMode(r, a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, rr, g, b, a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

static void draw_text(SDL_Renderer *r, TTF_Font *f, const char *txt,
                      int x, int y, Uint8 rr, Uint8 g, Uint8 b)
{
    if (!f || !txt || txt[0] == '\0') return;
    SDL_Color col = {rr, g, b, 255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, txt, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) {
        SDL_Rect dst = {x, y, surf->w, surf->h};
        SDL_RenderCopy(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

static void draw_text_fit(SDL_Renderer *r, TTF_Font *f, const char *txt,
                          int x, int y, int max_w, Uint8 rr, Uint8 g, Uint8 b)
{
    char buf[TITLE_LEN];
    copy_text(buf, sizeof(buf), txt);

    int tw = 0, th = 0;
    while (buf[0] && f && TTF_SizeUTF8(f, buf, &tw, &th) == 0 && tw > max_w) {
        size_t len = strlen(buf);
        if (len <= 4) break;
        buf[len - 1] = '\0';
    }
    if (txt && strlen(txt) > strlen(buf) && strlen(buf) > 3)
        copy_text(buf + strlen(buf) - 3, 4, "...");

    draw_text(r, f, buf, x, y, rr, g, b);
}

static void format_time(int seconds, char *out, size_t out_len)
{
    if (seconds <= 0) {
        copy_text(out, out_len, "0:00");
        return;
    }
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    if (h > 0)
        snprintf(out, out_len, "%d:%02d:%02d", h, m, s);
    else
        snprintf(out, out_len, "%d:%02d", m, s);
}

static int favorite_count(const Library *lib)
{
    int count = 0;
    for (int i = 0; i < lib->count; i++)
        if (lib->tracks[i].favorite)
            count++;
    return count;
}

static void draw_sidebar_button(SDL_Renderer *r, const UI *ui, const char *label,
                                int y, bool active)
{
    if (active)
        draw_rect(r, 16, y, SIDEBAR_W - 32, 38, 24, 42, 69, 255);
    draw_text_fit(r, ui->md, label, 30, y + 10, SIDEBAR_W - 60,
                  active ? 127 : 136, active ? 176 : 136, active ? 255 : 146);
}

static void render(SDL_Renderer *r, const UI *ui, Library *lib, const Player *player)
{
    draw_rect(r, 0, 0, win_w, win_h, 16, 17, 22, 255);

    int content_x = SIDEBAR_W;
    int content_w = win_w - SIDEBAR_W - NOW_W;
    if (content_w < 360) content_w = win_w - SIDEBAR_W;
    int now_x = SIDEBAR_W + content_w;
    int list_h = win_h - TOP_H - PLAYER_H;

    draw_rect(r, 0, 0, SIDEBAR_W, win_h - PLAYER_H, 26, 29, 37, 255);
    draw_rect(r, SIDEBAR_W - 2, 0, 2, win_h - PLAYER_H, 59, 66, 80, 255);
    draw_text(r, ui->lg, "ctify", 28, 28, 79, 142, 247);
    draw_sidebar_button(r, ui, "Home", 94, lib->view == VIEW_HOME);
    draw_sidebar_button(r, ui, "All Songs", 138, lib->view == VIEW_ALL);
    draw_rect(r, 20, 198, SIDEBAR_W - 40, 1, 42, 44, 51, 255);
    draw_text(r, ui->sm, "PLAYLISTS", 28, 210, 96, 100, 109);
    draw_sidebar_button(r, ui, "Favorites", 228, lib->view == VIEW_FAVORITES);

    draw_rect(r, content_x, 0, content_w, win_h - PLAYER_H, 16, 17, 22, 255);
    draw_rect(r, content_x, 0, content_w, TOP_H, 16, 17, 22, 255);
    const char *view_title = lib->view == VIEW_HOME ? "Home" :
                             lib->view == VIEW_FAVORITES ? "Favorites" : "All Songs";
    draw_text(r, ui->lg, view_title, content_x + LEFT_PAD, 24, 240, 240, 240);

    char info[256];
    snprintf(info, sizeof(info), "%d songs  |  %d favorites  |  / search",
             lib->count, favorite_count(lib));
    draw_text_fit(r, ui->sm, info, content_x + LEFT_PAD + 150, 34,
                  content_w - 180, 96, 100, 109);

    draw_rect(r, content_x, TOP_H - 1, content_w, 1, 42, 44, 51, 255);
    draw_text(r, ui->sm, "#", content_x + LEFT_PAD, TOP_H + 12, 68, 68, 74);
    draw_text(r, ui->sm, "TITLE", content_x + LEFT_PAD + 52, TOP_H + 12, 68, 68, 74);
    draw_text(r, ui->sm, "ARTIST", content_x + content_w - 330, TOP_H + 12, 68, 68, 74);
    draw_text(r, ui->sm, "TIME", content_x + content_w - 90, TOP_H + 12, 68, 68, 74);

    int table_top = TOP_H + 36;
    list_h = win_h - table_top - PLAYER_H;
    int total_visible = visible_count(lib);
    int content_h = total_visible * ROW_H;
    lib->max_scroll = content_h - list_h;
    if (lib->max_scroll < 0) lib->max_scroll = 0;
    if (lib->scroll_y > lib->max_scroll) lib->scroll_y = lib->max_scroll;

    SDL_Rect clip = {content_x, table_top, content_w, list_h};
    SDL_RenderSetClipRect(r, &clip);

    int pos = 0;
    for (int i = 0; i < lib->count; i++) {
        if (!matches_search(lib, i)) continue;
        int y = table_top + pos * ROW_H - lib->scroll_y;
        pos++;
        if (y + ROW_H < table_top || y > table_top + list_h) continue;

        bool selected = i == lib->selected;
        bool current = i == player->current;
        if (selected)
            draw_rect(r, content_x + 18, y + 3, content_w - 36, ROW_H - 6, 24, 32, 53, 255);
        if (current)
            draw_rect(r, content_x + 22, y + 8, 4, ROW_H - 16, 79, 142, 247, 255);

        const Track *t = &lib->tracks[i];
        char num[32];
        snprintf(num, sizeof(num), "%s%d", t->favorite ? "*" : "", pos);
        draw_text_fit(r, ui->sm, current ? ">" : num,
                      content_x + LEFT_PAD, y + 14, 42,
                      current ? 79 : 85, current ? 142 : 85, current ? 247 : 85);
        draw_text_fit(r, ui->md, t->title, content_x + LEFT_PAD + 52, y + 8,
                      content_w - 430, current ? 79 : 224, current ? 142 : 224, current ? 247 : 224);
        draw_text_fit(r, ui->sm, t->album[0] ? t->album : t->folder,
                      content_x + LEFT_PAD + 52, y + 27, content_w - 430, 96, 100, 109);
        draw_text_fit(r, ui->sm, t->artist, content_x + content_w - 330, y + 14,
                      190, 90, 143, 200);
        char dur[32];
        format_time(t->duration_sec, dur, sizeof(dur));
        draw_text_fit(r, ui->sm, dur, content_x + content_w - 90, y + 14,
                      70, 96, 100, 109);
    }

    SDL_RenderSetClipRect(r, NULL);

    if (lib->count == 0) {
        draw_text(r, ui->md, "No music found. Put audio files in ~/Music or run ctify /path/to/music.",
                  content_x + LEFT_PAD, table_top + 44, 174, 180, 190);
    } else if (total_visible == 0) {
        draw_text(r, ui->md, "No tracks match the search.",
                  content_x + LEFT_PAD, table_top + 44, 174, 180, 190);
    }

    if (now_x < win_w) {
        draw_rect(r, now_x, 0, win_w - now_x, win_h - PLAYER_H, 19, 21, 27, 255);
        draw_rect(r, now_x, 0, 2, win_h - PLAYER_H, 47, 53, 64, 255);
        draw_text(r, ui->sm, "NOW PLAYING", now_x + 24, 28, 96, 100, 109);
        draw_rect(r, now_x + 28, 70, NOW_W - 56, NOW_W - 56, 26, 42, 69, 255);
        const char *letter = "♪";
        char art_letter[8];
        if (player->current >= 0 && player->current < lib->count && lib->tracks[player->current].title[0]) {
            art_letter[0] = (char)toupper((unsigned char)lib->tracks[player->current].title[0]);
            art_letter[1] = '\0';
            letter = art_letter;
        }
        draw_text(r, ui->lg, letter, now_x + NOW_W / 2 - 10, 150, 130, 160, 210);
    }

    draw_rect(r, 0, win_h - PLAYER_H, win_w, PLAYER_H, 27, 30, 39, 255);
    draw_rect(r, 0, win_h - PLAYER_H, win_w, 2, 61, 69, 84, 255);

    const char *title = "Nothing playing";
    const char *artist = "";
    const char *album = "";
    if (player->current >= 0 && player->current < lib->count) {
        title = lib->tracks[player->current].title;
        artist = lib->tracks[player->current].artist;
        album = lib->tracks[player->current].album;
    }

    draw_text_fit(r, ui->md, title, 28, win_h - PLAYER_H + 28,
                  230, 240, 240, 240);
    draw_text_fit(r, ui->sm, artist, 28, win_h - PLAYER_H + 56,
                  230, 102, 102, 102);

    int cx = win_w / 2;
    draw_text(r, ui->md, player->shuffle ? "SHUF" : "shuf", cx - 180, win_h - PLAYER_H + 28,
              player->shuffle ? 79 : 160, player->shuffle ? 142 : 160, player->shuffle ? 247 : 160);
    draw_text(r, ui->md, "<<", cx - 92, win_h - PLAYER_H + 28, 180, 180, 185);
    draw_rect(r, cx - 30, win_h - PLAYER_H + 18, 60, 44, 79, 142, 247, 255);
    draw_text(r, ui->md, player->playing ? "II" : ">", cx - 6, win_h - PLAYER_H + 31, 6, 17, 33);
    draw_text(r, ui->md, ">>", cx + 70, win_h - PLAYER_H + 28, 180, 180, 185);
    draw_text(r, ui->md, player->repeat ? "REP" : "rep", cx + 150, win_h - PLAYER_H + 28,
              player->repeat ? 79 : 160, player->repeat ? 142 : 160, player->repeat ? 247 : 160);

    draw_rect(r, cx - 220, win_h - PLAYER_H + 78, 440, 4, 48, 51, 58, 255);
    draw_rect(r, cx - 220, win_h - PLAYER_H + 78, player->playing ? 110 : 8, 4, 232, 237, 245, 255);
    draw_text_fit(r, ui->sm, album, win_w - 260, win_h - PLAYER_H + 30, 140, 96, 100, 109);
    char vol[64];
    snprintf(vol, sizeof(vol), "Volume %d%%", player->volume);
    draw_text_fit(r, ui->sm, vol, win_w - 150, win_h - PLAYER_H + 58, 120, 96, 100, 109);

    if (lib->search_active) {
        int w = 520, h = 46;
        int x = (win_w - w) / 2;
        int y = win_h - PLAYER_H - h - 18;
        draw_rect(r, x - 4, y - 4, w + 8, h + 8, 0, 0, 0, 170);
        draw_rect(r, x, y, w, h, 28, 30, 38, 255);
        char q[320];
        snprintf(q, sizeof(q), "Search: %s|", lib->search);
        draw_text_fit(r, ui->md, q, x + 14, y + 12, w - 28, 238, 238, 240);
    }
}

static void init_default_root(char *out, size_t out_len)
{
    const char *home = getenv("HOME");
    if (home && home[0] != '\0')
        path_join(out, out_len, home, "Music");
    else
        copy_text(out, out_len, "Music");
}

static void play_selected(Player *player, const Library *lib)
{
    if (lib->selected < 0) return;
    if (player->current == lib->selected && player->pid > 0) {
        player_pause_toggle(player);
    } else {
        player_play(player, lib, lib->selected);
    }
}

static void play_relative(Player *player, Library *lib, int dir)
{
    int start = player->current >= 0 ? player->current : lib->selected;
    int next = player->shuffle ? random_visible(lib) : visible_next(lib, start, dir);
    if (next >= 0) {
        lib->selected = next;
        ensure_selected_visible(lib);
        player_play(player, lib, next);
    }
}

static void toggle_favorite(Library *lib)
{
    if (lib->selected < 0 || lib->selected >= lib->count) return;
    lib->tracks[lib->selected].favorite = !lib->tracks[lib->selected].favorite;
    save_favorites(lib);
    if (lib->view == VIEW_FAVORITES && !lib->tracks[lib->selected].favorite)
        select_first_visible(lib);
}

int main(int argc, char **argv)
{
    srand((unsigned int)time(NULL));
    if (!command_exists("mpv")) {
        fprintf(stderr, "ctify: mpv is required for playback\n");
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(APP_TITLE, SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    UI ui = {load_font(13), load_font(16), load_font(25)};
    Library *lib = calloc(1, sizeof(*lib));
    if (!lib) {
        fprintf(stderr, "Out of memory\n");
        ui_close(&ui);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    if (argc >= 2) copy_text(lib->root, sizeof(lib->root), argv[1]);
    else init_default_root(lib->root, sizeof(lib->root));
    library_scan(lib);

    Player player = {.pid = -1, .current = -1, .playing = false, .volume = 70};
    SDL_StartTextInput();

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_WINDOWEVENT &&
                       ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = ev.window.data1;
                win_h = ev.window.data2;
            } else if (ev.type == SDL_MOUSEWHEEL) {
                lib->scroll_y -= ev.wheel.y * ROW_H;
                if (lib->scroll_y < 0) lib->scroll_y = 0;
                if (lib->scroll_y > lib->max_scroll) lib->scroll_y = lib->max_scroll;
            } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                int nav = sidebar_hit(ev.button.x, ev.button.y);
                if (nav >= 0) {
                    lib->view = (ViewMode)nav;
                    lib->scroll_y = 0;
                    select_first_visible(lib);
                } else {
                    int idx = hit_test(lib, ev.button.x, ev.button.y);
                    if (idx >= 0) {
                    lib->selected = idx;
                    play_selected(&player, lib);
                    }
                }
            } else if (ev.type == SDL_TEXTINPUT && lib->search_active) {
                if (lib->ignore_next_text) {
                    lib->ignore_next_text = false;
                } else {
                    size_t len = strlen(lib->search);
                    if (len + strlen(ev.text.text) < sizeof(lib->search))
                        strcat(lib->search, ev.text.text);
                    select_first_visible(lib);
                    ensure_selected_visible(lib);
                }
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode key = ev.key.keysym.sym;
                if (lib->search_active) {
                    if (key == SDLK_ESCAPE) {
                        lib->search_active = false;
                        lib->search[0] = '\0';
                        select_first_visible(lib);
                    } else if (key == SDLK_BACKSPACE) {
                        size_t len = strlen(lib->search);
                        if (len > 0) lib->search[len - 1] = '\0';
                        select_first_visible(lib);
                    } else if (key == SDLK_RETURN) {
                        lib->search_active = false;
                    }
                } else if (key == SDLK_ESCAPE) {
                    running = false;
                } else if (key == SDLK_SLASH) {
                    lib->search_active = true;
                    lib->search[0] = '\0';
                    lib->ignore_next_text = true;
                } else if (key == SDLK_r) {
                    library_scan(lib);
                } else if (key == SDLK_SPACE || key == SDLK_RETURN) {
                    play_selected(&player, lib);
                } else if (key == SDLK_n) {
                    play_relative(&player, lib, 1);
                } else if (key == SDLK_b) {
                    play_relative(&player, lib, -1);
                } else if (key == SDLK_f) {
                    toggle_favorite(lib);
                } else if (key == SDLK_s) {
                    player.shuffle = !player.shuffle;
                } else if (key == SDLK_t) {
                    player.repeat = !player.repeat;
                } else if (key == SDLK_1) {
                    lib->view = VIEW_HOME;
                    lib->scroll_y = 0;
                    select_first_visible(lib);
                } else if (key == SDLK_2) {
                    lib->view = VIEW_ALL;
                    lib->scroll_y = 0;
                    select_first_visible(lib);
                } else if (key == SDLK_3) {
                    lib->view = VIEW_FAVORITES;
                    lib->scroll_y = 0;
                    select_first_visible(lib);
                } else if (key == SDLK_EQUALS || key == SDLK_PLUS) {
                    player_set_volume(&player, player.volume + 5);
                } else if (key == SDLK_MINUS) {
                    player_set_volume(&player, player.volume - 5);
                } else if (key == SDLK_RIGHT || key == SDLK_DOWN) {
                    lib->selected = visible_next(lib, lib->selected, 1);
                    ensure_selected_visible(lib);
                } else if (key == SDLK_LEFT || key == SDLK_UP) {
                    lib->selected = visible_next(lib, lib->selected, -1);
                    ensure_selected_visible(lib);
                } else if (key == SDLK_PAGEDOWN) {
                    lib->scroll_y += win_h - TOP_H - PLAYER_H;
                    if (lib->scroll_y > lib->max_scroll) lib->scroll_y = lib->max_scroll;
                } else if (key == SDLK_PAGEUP) {
                    lib->scroll_y -= win_h - TOP_H - PLAYER_H;
                    if (lib->scroll_y < 0) lib->scroll_y = 0;
                }
            }
        }

        player_reap(&player, lib);
        render(renderer, &ui, lib, &player);
        SDL_RenderPresent(renderer);
    }

    SDL_StopTextInput();
    player_stop(&player);
    free(lib);
    ui_close(&ui);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
