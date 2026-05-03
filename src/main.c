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
#define ROW_H 42
#define TOP_H 92
#define PLAYER_H 118
#define LEFT_PAD 34
#define MAX_PLAYERS 1

typedef struct {
    char path[PATH_MAX];
    char title[TITLE_LEN];
    char folder[ARTIST_LEN];
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

static void title_from_filename(const char *name, char *out, size_t out_len)
{
    copy_text(out, out_len, name);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    for (char *p = out; *p; p++) {
        if (*p == '_' || *p == '.') *p = ' ';
    }
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
    int folder = strcasecmp(ta->folder, tb->folder);
    if (folder != 0) return folder;
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
    title_from_filename(basename_of(path), t->title, sizeof(t->title));
    copy_text(t->folder, sizeof(t->folder), dirname_label(path));
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
    lib->selected = lib->count > 0 ? 0 : -1;
}

static bool matches_search(const Library *lib, int idx)
{
    if (idx < 0 || idx >= lib->count) return false;
    if (lib->search[0] == '\0') return true;
    const Track *t = &lib->tracks[idx];
    return strcasestr(t->title, lib->search) ||
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
    (void)mx;
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

static void player_reap(Player *p)
{
    if (p->pid <= 0) return;
    int status = 0;
    pid_t done = waitpid(p->pid, &status, WNOHANG);
    if (done == p->pid) {
        p->pid = -1;
        p->playing = false;
        if (p->socket_path[0] != '\0') {
            unlink(p->socket_path);
            p->socket_path[0] = '\0';
        }
    }
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
        copy_text(buf + len - 3, 4, "...");
    }

    draw_text(r, f, buf, x, y, rr, g, b);
}

static void render(SDL_Renderer *r, const UI *ui, Library *lib, const Player *player)
{
    draw_rect(r, 0, 0, win_w, win_h, 11, 13, 16, 255);
    draw_rect(r, 0, 0, win_w, TOP_H, 16, 18, 22, 255);
    draw_rect(r, 0, TOP_H - 1, win_w, 1, 29, 185, 84, 180);

    draw_text(r, ui->lg, "ctify", LEFT_PAD, 24, 29, 185, 84);

    char info[512];
    char root_label[TITLE_LEN];
    copy_text(root_label, sizeof(root_label), lib->root);
    snprintf(info, sizeof(info),
             "%d tracks  |  root: %s  |  Space play/pause  n next  b prev  +/- volume  / search",
             lib->count, root_label);
    draw_text_fit(r, ui->sm, info, 150, 36, win_w - 180, 174, 180, 190);

    int list_h = win_h - TOP_H - PLAYER_H;
    int total_visible = visible_count(lib);
    int content_h = total_visible * ROW_H;
    lib->max_scroll = content_h - list_h;
    if (lib->max_scroll < 0) lib->max_scroll = 0;
    if (lib->scroll_y > lib->max_scroll) lib->scroll_y = lib->max_scroll;

    SDL_Rect clip = {0, TOP_H, win_w, list_h};
    SDL_RenderSetClipRect(r, &clip);

    int pos = 0;
    for (int i = 0; i < lib->count; i++) {
        if (!matches_search(lib, i)) continue;
        int y = TOP_H + pos * ROW_H - lib->scroll_y;
        pos++;
        if (y + ROW_H < TOP_H || y > TOP_H + list_h) continue;

        bool selected = i == lib->selected;
        bool current = i == player->current;
        if (selected)
            draw_rect(r, 20, y + 4, win_w - 40, ROW_H - 8, 31, 41, 52, 255);
        if (current)
            draw_rect(r, 24, y + 8, 4, ROW_H - 16, 29, 185, 84, 255);

        const Track *t = &lib->tracks[i];
        draw_text_fit(r, ui->md, t->title, LEFT_PAD + 10, y + 9,
                      win_w - 360, selected ? 245 : 224, selected ? 247 : 228, selected ? 250 : 234);
        draw_text_fit(r, ui->sm, t->folder, win_w - 280, y + 13,
                      220, 137, 144, 154);
    }

    SDL_RenderSetClipRect(r, NULL);

    if (lib->count == 0) {
        draw_text(r, ui->md, "No music found. Put audio files in ~/Music or run ctify /path/to/music.",
                  LEFT_PAD, TOP_H + 44, 174, 180, 190);
    } else if (total_visible == 0) {
        draw_text(r, ui->md, "No tracks match the search.",
                  LEFT_PAD, TOP_H + 44, 174, 180, 190);
    }

    draw_rect(r, 0, win_h - PLAYER_H, win_w, PLAYER_H, 16, 18, 22, 255);
    draw_rect(r, 0, win_h - PLAYER_H, win_w, 1, 40, 45, 54, 255);

    const char *title = "Nothing playing";
    const char *folder = "";
    if (player->current >= 0 && player->current < lib->count) {
        title = lib->tracks[player->current].title;
        folder = lib->tracks[player->current].folder;
    }

    draw_text_fit(r, ui->md, title, LEFT_PAD, win_h - PLAYER_H + 28,
                  win_w - 360, 245, 247, 250);
    draw_text_fit(r, ui->sm, folder, LEFT_PAD, win_h - PLAYER_H + 58,
                  win_w - 360, 137, 144, 154);

    char controls[160];
    snprintf(controls, sizeof(controls), "[%s]  Prev  Play/Pause  Next     Volume %d%%",
             player->playing ? "playing" : "paused", player->volume);
    draw_text_fit(r, ui->md, controls, win_w - 430, win_h - PLAYER_H + 42,
                  380, 174, 180, 190);

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
    int next = visible_next(lib, start, dir);
    if (next >= 0) {
        lib->selected = next;
        ensure_selected_visible(lib);
        player_play(player, lib, next);
    }
}

int main(int argc, char **argv)
{
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
                int idx = hit_test(lib, ev.button.x, ev.button.y);
                if (idx >= 0) {
                    lib->selected = idx;
                    play_selected(&player, lib);
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

        player_reap(&player);
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
