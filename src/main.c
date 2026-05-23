#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <sqlite3.h>
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
#define MAX_PLAYLISTS 32
#define MAX_PLAYLIST_TRACKS 256
#define MAX_FOLDERS 64

enum {
    CTRL_NONE = 0,
    CTRL_SHUFFLE,
    CTRL_PREV,
    CTRL_PLAY,
    CTRL_NEXT,
    CTRL_REPEAT
};

#define NAV_ADD_MUSIC 98
#define NAV_NEW_PLAYLIST 99
#define NAV_REFRESH 97

typedef enum {
    VIEW_HOME = 0,
    VIEW_ALL = 1,
    VIEW_FAVORITES = 2,
    VIEW_PLAYLIST = 3,
    VIEW_FOLDER = 4,
    VIEW_ARTIST = 5,
    VIEW_ALBUM = 6
} ViewMode;

typedef struct {
    char path[PATH_MAX];
    char title[TITLE_LEN];
    char artist[ARTIST_LEN];
    char album[ALBUM_LEN];
    char folder[ARTIST_LEN];
    int duration_sec;
    int track_no;
    double bpm;
    bool favorite;
} Track;

typedef struct {
    char name[TITLE_LEN];
    char path[PATH_MAX];
    char tracks[MAX_PLAYLIST_TRACKS][PATH_MAX];
    int count;
} Playlist;

typedef struct {
    Track tracks[MAX_TRACKS];
    int count;
    Playlist playlists[MAX_PLAYLISTS];
    int playlist_count;
    int active_playlist;
    int selected;
    int hovered;
    int hover_control;
    int hover_nav;
    int hover_playlist;
    bool playlist_name_active;
    char playlist_name[TITLE_LEN];
    bool menu_open;
    int menu_x;
    int menu_y;
    int menu_track;
    int menu_hover;
    int scroll_y;
    int max_scroll;
    char root[PATH_MAX];
    char folders[MAX_FOLDERS][PATH_MAX];
    int folder_count;
    char search[TITLE_LEN];
    char filter[TITLE_LEN];
    bool search_active;
    bool ignore_next_text;
    bool bpm_sort;
    ViewMode view;
    SDL_Texture *covers[MAX_TRACKS];
    int cover_cursor;
    bool covers_loaded;
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
    int duration_sec;
    double paused_pos;
    Uint32 started_ticks;
    bool was_paused;
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

static unsigned long hash_string(const char *s)
{
    unsigned long hash = 1469598103934665603UL;
    while (*s) {
        hash ^= (unsigned char)*s++;
        hash *= 1099511628211UL;
    }
    return hash;
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

static void dirname_path(const char *path, char *out, size_t out_len)
{
    copy_text(out, out_len, path);
    char *slash = strrchr(out, '/');
    if (slash && slash != out)
        *slash = '\0';
    else if (slash)
        slash[1] = '\0';
    else
        copy_text(out, out_len, ".");
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

static void playlists_dir(char *out, size_t out_len)
{
    char state_dir[PATH_MAX];
    ensure_state_dir(state_dir, sizeof(state_dir));
    if (path_join(out, out_len, state_dir, "playlists"))
        mkdir(out, 0755);
}

static void playlist_file(char *out, size_t out_len, const char *name)
{
    char dir[PATH_MAX];
    char file[TITLE_LEN + 8];
    playlists_dir(dir, sizeof(dir));
    char safe[TITLE_LEN];
    copy_text(safe, sizeof(safe), name);
    for (char *p = safe; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            *p = '_';
    snprintf(file, sizeof(file), "%s.m3u", safe);
    path_join(out, out_len, dir, file);
}

static void covers_dir(char *out, size_t out_len)
{
    char state_dir[PATH_MAX];
    ensure_state_dir(state_dir, sizeof(state_dir));
    if (path_join(out, out_len, state_dir, "covers"))
        mkdir(out, 0755);
}

static void db_file(char *out, size_t out_len)
{
    char state_dir[PATH_MAX];
    ensure_state_dir(state_dir, sizeof(state_dir));
    path_join(out, out_len, state_dir, "library.db");
}

static sqlite3 *db_open(void)
{
    char path[PATH_MAX];
    db_file(path, sizeof(path));
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        return NULL;

    sqlite3_exec(db,
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS tracks ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "path TEXT UNIQUE NOT NULL,"
        "title TEXT, artist TEXT, album TEXT,"
        "duration INTEGER DEFAULT 0,"
        "bpm REAL DEFAULT 0,"
        "folder TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS library_folders ("
        "path TEXT PRIMARY KEY"
        ");",
        NULL, NULL, NULL);
    return db;
}

static void db_add_folder(const char *path)
{
    sqlite3 *db = db_open();
    if (!db) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO library_folders(path) VALUES(?)",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static void db_load_folders(Library *lib)
{
    lib->folder_count = 0;
    sqlite3 *db = db_open();
    if (!db) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT path FROM library_folders ORDER BY path",
                           -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW && lib->folder_count < MAX_FOLDERS) {
            const unsigned char *p = sqlite3_column_text(st, 0);
            if (p && p[0])
                copy_text(lib->folders[lib->folder_count++], PATH_MAX, (const char *)p);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static void db_save_track(const Track *t)
{
    sqlite3 *db = db_open();
    if (!db) return;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO tracks(path,title,artist,album,duration,bpm,folder) "
        "VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "title=excluded.title, artist=excluded.artist, album=excluded.album, "
        "duration=excluded.duration, bpm=excluded.bpm, folder=excluded.folder";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, t->path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, t->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, t->artist, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, t->album, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 5, t->duration_sec);
        sqlite3_bind_double(st, 6, t->bpm);
        sqlite3_bind_text(st, 7, t->folder, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static void db_remove_track(const char *path)
{
    sqlite3 *db = db_open();
    if (!db) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM tracks WHERE path=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
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
    t->track_no = 0;
    t->bpm = 0.0;
}

static void load_metadata(Track *t)
{
    metadata_from_filename(t);
    if (!command_exists("ffprobe")) return;

    char quoted[PATH_MAX + 64];
    shell_quote(quoted, sizeof(quoted), t->path);
    char cmd[PATH_MAX + 512];
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -show_entries format=duration:format_tags=title,artist,album,track,TRACKNUMBER,bpm,BPM,TBPM -of default=nw=1:nk=0 %s",
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
        else if ((strncmp(line, "TAG:track=", 10) == 0 ||
                  strncmp(line, "TAG:TRACKNUMBER=", 16) == 0)) {
            const char *v = strchr(line, '=');
            if (v && v[1]) t->track_no = atoi(v + 1);
        }
        else if ((strncmp(line, "TAG:bpm=", 8) == 0 ||
                  strncmp(line, "TAG:BPM=", 8) == 0 ||
                  strncmp(line, "TAG:TBPM=", 9) == 0)) {
            const char *v = strchr(line, '=');
            if (v && v[1]) t->bpm = atof(v + 1);
        }
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
    if (ta->track_no > 0 || tb->track_no > 0) {
        if (ta->track_no <= 0) return 1;
        if (tb->track_no <= 0) return -1;
        if (ta->track_no != tb->track_no)
            return ta->track_no - tb->track_no;
    }
    int title = strcasecmp(ta->title, tb->title);
    if (title != 0) return title;
    return strcasecmp(ta->path, tb->path);
}

static int cmp_track_bpm(const void *a, const void *b)
{
    const Track *ta = (const Track *)a;
    const Track *tb = (const Track *)b;
    if (ta->bpm <= 0.0 && tb->bpm > 0.0) return 1;
    if (tb->bpm <= 0.0 && ta->bpm > 0.0) return -1;
    if (ta->bpm < tb->bpm) return -1;
    if (ta->bpm > tb->bpm) return 1;
    return cmp_track(a, b);
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
    db_save_track(t);
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

static void library_free_covers(Library *lib)
{
    if (!lib) return;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (lib->covers[i]) {
            SDL_DestroyTexture(lib->covers[i]);
            lib->covers[i] = NULL;
        }
    }
    lib->cover_cursor = 0;
    lib->covers_loaded = false;
}

static void load_playlists(Library *lib)
{
    lib->playlist_count = 0;
    lib->active_playlist = -1;

    char dir_path[PATH_MAX];
    playlists_dir(dir_path, sizeof(dir_path));
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && lib->playlist_count < MAX_PLAYLISTS) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcasecmp(ext, ".m3u") != 0) continue;

        Playlist *pl = &lib->playlists[lib->playlist_count];
        memset(pl, 0, sizeof(*pl));
        copy_text(pl->name, sizeof(pl->name), entry->d_name);
        char *dot = strrchr(pl->name, '.');
        if (dot) *dot = '\0';
        if (!path_join(pl->path, sizeof(pl->path), dir_path, entry->d_name))
            continue;

        FILE *fp = fopen(pl->path, "r");
        if (!fp) continue;
        char line[PATH_MAX + 8];
        while (fgets(line, sizeof(line), fp) && pl->count < MAX_PLAYLIST_TRACKS) {
            trim_line(line);
            if (line[0] == '\0' || line[0] == '#') continue;
            copy_text(pl->tracks[pl->count++], PATH_MAX, line);
        }
        fclose(fp);
        lib->playlist_count++;
    }
    closedir(dir);
}

static bool playlist_contains(const Playlist *pl, const char *track_path)
{
    if (!pl) return false;
    for (int i = 0; i < pl->count; i++)
        if (strcmp(pl->tracks[i], track_path) == 0)
            return true;
    return false;
}

static bool matches_search(const Library *lib, int idx);
static void select_first_visible(Library *lib);
static void library_scan(Library *lib);

static bool playlist_name_exists(const Library *lib, const char *name)
{
    for (int i = 0; i < lib->playlist_count; i++)
        if (strcasecmp(lib->playlists[i].name, name) == 0)
            return true;
    return false;
}

static int create_playlist_named(Library *lib, const char *wanted_name)
{
    if (lib->playlist_count >= MAX_PLAYLISTS) return -1;

    char name[TITLE_LEN];
    copy_text(name, sizeof(name), wanted_name && wanted_name[0] ? wanted_name : "Playlist");
    trim_line(name);
    char base[TITLE_LEN - 16];
    copy_text(base, sizeof(base), name);
    for (int n = 2; playlist_name_exists(lib, name) && n < 1000; n++)
        snprintf(name, sizeof(name), "%s %d", base, n);
    if (playlist_name_exists(lib, name)) return -1;

    Playlist *pl = &lib->playlists[lib->playlist_count];
    memset(pl, 0, sizeof(*pl));
    copy_text(pl->name, sizeof(pl->name), name);
    playlist_file(pl->path, sizeof(pl->path), name);
    FILE *fp = fopen(pl->path, "w");
    if (!fp) return -1;
    fclose(fp);
    return lib->playlist_count++;
}

static void add_to_playlist(Library *lib, int playlist_idx, int track_idx)
{
    if (playlist_idx < 0 || playlist_idx >= lib->playlist_count) return;
    if (track_idx < 0 || track_idx >= lib->count) return;
    Playlist *pl = &lib->playlists[playlist_idx];
    if (pl->count >= MAX_PLAYLIST_TRACKS || playlist_contains(pl, lib->tracks[track_idx].path))
        return;
    copy_text(pl->tracks[pl->count++], PATH_MAX, lib->tracks[track_idx].path);

    FILE *fp = fopen(pl->path, "a");
    if (!fp) return;
    fprintf(fp, "%s\n", lib->tracks[track_idx].path);
    fclose(fp);
}

static void add_visible_to_playlist(Library *lib, int playlist_idx)
{
    if (playlist_idx < 0 || playlist_idx >= lib->playlist_count) return;
    for (int i = 0; i < lib->count; i++)
        if (matches_search(lib, i))
            add_to_playlist(lib, playlist_idx, i);
}

static void delete_playlist(Library *lib, int playlist_idx)
{
    if (playlist_idx < 0 || playlist_idx >= lib->playlist_count) return;
    unlink(lib->playlists[playlist_idx].path);
    for (int i = playlist_idx; i + 1 < lib->playlist_count; i++)
        lib->playlists[i] = lib->playlists[i + 1];
    lib->playlist_count--;
    if (lib->active_playlist == playlist_idx) {
        lib->view = VIEW_HOME;
        lib->active_playlist = -1;
        lib->scroll_y = 0;
        select_first_visible(lib);
    } else if (lib->active_playlist > playlist_idx) {
        lib->active_playlist--;
    }
}

static void rewrite_playlist(const Playlist *pl)
{
    if (!pl) return;
    FILE *fp = fopen(pl->path, "w");
    if (!fp) return;
    for (int i = 0; i < pl->count; i++)
        fprintf(fp, "%s\n", pl->tracks[i]);
    fclose(fp);
}

static void remove_from_playlist(Library *lib, int playlist_idx, int track_idx)
{
    if (playlist_idx < 0 || playlist_idx >= lib->playlist_count) return;
    if (track_idx < 0 || track_idx >= lib->count) return;
    Playlist *pl = &lib->playlists[playlist_idx];
    for (int i = 0; i < pl->count; i++) {
        if (strcmp(pl->tracks[i], lib->tracks[track_idx].path) != 0) continue;
        for (int j = i; j + 1 < pl->count; j++)
            copy_text(pl->tracks[j], PATH_MAX, pl->tracks[j + 1]);
        pl->count--;
        rewrite_playlist(pl);
        if (lib->view == VIEW_PLAYLIST)
            select_first_visible(lib);
        return;
    }
}

static int ensure_default_playlist(Library *lib)
{
    for (int i = 0; i < lib->playlist_count; i++)
        if (strcmp(lib->playlists[i].name, "Library") == 0)
            return i;
    if (lib->playlist_count >= MAX_PLAYLISTS) return -1;

    Playlist *pl = &lib->playlists[lib->playlist_count];
    memset(pl, 0, sizeof(*pl));
    copy_text(pl->name, sizeof(pl->name), "Library");
    playlist_file(pl->path, sizeof(pl->path), "Library");
    FILE *fp = fopen(pl->path, "a");
    if (fp) fclose(fp);
    return lib->playlist_count++;
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

static int import_audio_file(Library *lib, const char *path)
{
    if (!path || path[0] == '\0') return 0;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || !is_audio(path))
        return 0;
    if (track_exists(lib, path))
        return 0;
    char parent[PATH_MAX];
    dirname_path(path, parent, sizeof(parent));
    db_add_folder(parent);
    if (lib->folder_count < MAX_FOLDERS && !track_exists(lib, path)) {
        bool known_folder = false;
        for (int i = 0; i < lib->folder_count; i++) {
            if (strcmp(lib->folders[i], parent) == 0) {
                known_folder = true;
                break;
            }
        }
        if (!known_folder)
            copy_text(lib->folders[lib->folder_count++], PATH_MAX, parent);
    }
    add_track(lib, path);
    return 1;
}

static int import_audio_files_with_picker(Library *lib)
{
    if (!command_exists("zenity")) {
        fprintf(stderr, "ctify: zenity is required for the file picker\n");
        return 0;
    }

    const char *cmd =
        "zenity --file-selection --multiple --separator='|' "
        "--title='Add music files' "
        "--file-filter='Audio files | *.mp3 *.flac *.ogg *.m4a *.wav *.aac *.opus *.wma'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    char buf[16384];
    size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[len] = '\0';
    pclose(fp);
    trim_line(buf);
    if (buf[0] == '\0') return 0;

    int imported = 0;
    char *save = NULL;
    for (char *path = strtok_r(buf, "|", &save); path; path = strtok_r(NULL, "|", &save)) {
        trim_line(path);
        imported += import_audio_file(lib, path);
    }
    if (imported > 0) {
        library_scan(lib);
        select_first_visible(lib);
    }
    return imported;
}

static void library_scan(Library *lib)
{
    library_free_covers(lib);
    lib->count = 0;
    lib->selected = -1;
    lib->hovered = -1;
    lib->hover_control = CTRL_NONE;
    lib->hover_nav = -1;
    lib->hover_playlist = -1;
    lib->scroll_y = 0;
    lib->max_scroll = 0;

    db_add_folder(lib->root);
    db_load_folders(lib);
    if (lib->folder_count == 0) {
        copy_text(lib->folders[lib->folder_count++], PATH_MAX, lib->root);
    }
    for (int i = 0; i < lib->folder_count; i++)
        scan_dir(lib, lib->folders[i], 0);
    qsort(lib->tracks, (size_t)lib->count, sizeof(Track),
          lib->bpm_sort ? cmp_track_bpm : cmp_track);
    load_favorites(lib);
    load_playlists(lib);
    lib->selected = lib->count > 0 ? 0 : -1;
}

static bool matches_search(const Library *lib, int idx)
{
    if (idx < 0 || idx >= lib->count) return false;
    const Track *t = &lib->tracks[idx];
    if (lib->view == VIEW_FAVORITES && !t->favorite) return false;
    if (lib->view == VIEW_FOLDER) {
        size_t n = strlen(lib->filter);
        if (n == 0 || strncmp(t->path, lib->filter, n) != 0 ||
            (t->path[n] != '/' && t->path[n] != '\0'))
            return false;
    }
    if (lib->view == VIEW_ARTIST && strcasecmp(t->artist, lib->filter) != 0)
        return false;
    if (lib->view == VIEW_ALBUM && strcasecmp(t->album, lib->filter) != 0)
        return false;
    if (lib->view == VIEW_PLAYLIST) {
        if (lib->active_playlist < 0 || lib->active_playlist >= lib->playlist_count)
            return false;
        if (!playlist_contains(&lib->playlists[lib->active_playlist], t->path))
            return false;
    }
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

static int folder_track_count(const Library *lib, const char *folder)
{
    int count = 0;
    size_t n = strlen(folder);
    for (int i = 0; i < lib->count; i++)
        if (strncmp(lib->tracks[i].path, folder, n) == 0 &&
            (lib->tracks[i].path[n] == '/' || lib->tracks[i].path[n] == '\0'))
            count++;
    return count;
}

static int folder_favorite_count(const Library *lib, const char *folder)
{
    int count = 0;
    size_t n = strlen(folder);
    for (int i = 0; i < lib->count; i++)
        if (lib->tracks[i].favorite &&
            strncmp(lib->tracks[i].path, folder, n) == 0 &&
            (lib->tracks[i].path[n] == '/' || lib->tracks[i].path[n] == '\0'))
            count++;
    return count;
}

static bool artist_seen_before(const Library *lib, int idx)
{
    for (int i = 0; i < idx; i++)
        if (lib->tracks[i].artist[0] && strcasecmp(lib->tracks[i].artist, lib->tracks[idx].artist) == 0)
            return true;
    return false;
}

static int artist_track_count(const Library *lib, const char *artist)
{
    int count = 0;
    for (int i = 0; i < lib->count; i++)
        if (strcasecmp(lib->tracks[i].artist, artist) == 0)
            count++;
    return count;
}

static int home_hit(Library *lib, int mx, int my, char *filter, size_t filter_len)
{
    int content_x = SIDEBAR_W;
    int content_w = win_w - SIDEBAR_W - NOW_W;
    if (content_w < 360) content_w = win_w - SIDEBAR_W;
    int x0 = content_x + LEFT_PAD;
    int card = 148;
    int gap = 18;
    int cols = (content_w - LEFT_PAD * 2 + gap) / (card + gap);
    if (cols < 1) cols = 1;

    int y = TOP_H + 58;
    for (int i = 0; i < lib->folder_count; i++) {
        int row = i / cols, col = i % cols;
        int x = x0 + col * (card + gap);
        int cy = y + row * 206;
        if (mx >= x && mx < x + card && my >= cy && my < cy + 190) {
            copy_text(filter, filter_len, lib->folders[i]);
            return VIEW_FOLDER;
        }
    }

    y += ((lib->folder_count + cols - 1) / cols) * 206 + 44;
    int a = 0;
    for (int i = 0; i < lib->count; i++) {
        if (!lib->tracks[i].artist[0] || artist_seen_before(lib, i)) continue;
        int row = a / cols, col = a % cols;
        int x = x0 + col * (card + gap);
        int cy = y + row * 206;
        if (mx >= x && mx < x + card && my >= cy && my < cy + 190) {
            copy_text(filter, filter_len, lib->tracks[i].artist);
            return VIEW_ARTIST;
        }
        a++;
    }
    return -1;
}

static int hit_test(const Library *lib, int mx, int my)
{
    int content_x = SIDEBAR_W;
    int content_w = win_w - SIDEBAR_W - NOW_W;
    if (content_w < 360) content_w = win_w - SIDEBAR_W;
    int table_top = TOP_H + 36;
    int list_h = win_h - table_top - PLAYER_H;
    if (mx < content_x || mx >= content_x + content_w) return -1;
    if (my < table_top || my >= table_top + list_h) return -1;

    int visible_idx = (my - table_top + lib->scroll_y) / ROW_H;
    int pos = 0;
    for (int i = 0; i < lib->count; i++) {
        if (!matches_search(lib, i)) continue;
        if (pos == visible_idx) return i;
        pos++;
    }
    return -1;
}

static int sidebar_hit(const Library *lib, int mx, int my, int *playlist_idx)
{
    if (playlist_idx) *playlist_idx = -1;
    if (mx < 0 || mx >= SIDEBAR_W || my < 0 || my >= win_h - PLAYER_H)
        return -1;
    if (my >= 94 && my < 132) return VIEW_HOME;
    if (my >= 138 && my < 176) return VIEW_ALL;
    if (my >= 182 && my < 220) return NAV_ADD_MUSIC;
    if (my >= 226 && my < 264) return NAV_REFRESH;
    if (my >= 310 && my < 348) return VIEW_FAVORITES;
    int first = 354;
    for (int i = 0; i < lib->playlist_count; i++) {
        int y = first + i * 40;
        if (my >= y && my < y + 36) {
            if (playlist_idx) *playlist_idx = i;
            return VIEW_PLAYLIST;
        }
    }
    return -1;
}

static bool new_playlist_hit(int mx, int my)
{
    return mx >= SIDEBAR_W - 54 && mx < SIDEBAR_W - 24 && my >= 274 && my < 300;
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

static double player_position(const Player *p);

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
        p->duration_sec = lib->tracks[idx].duration_sec;
        p->paused_pos = 0.0;
        p->started_ticks = SDL_GetTicks();
    }
}

static void player_pause_toggle(Player *p)
{
    if (p->pid <= 0) return;
    ipc_send(p, "{\"command\":[\"cycle\",\"pause\"]}");
    if (p->playing) {
        p->paused_pos = player_position(p);
        p->playing = false;
    } else {
        p->started_ticks = SDL_GetTicks();
        p->playing = true;
    }
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
        p->paused_pos = 0.0;
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

static bool make_cover_bmp(const Track *t, char *bmp_path, size_t path_len)
{
    char dir[PATH_MAX];
    covers_dir(dir, sizeof(dir));

    struct stat st;
    long mtime = stat(t->path, &st) == 0 ? (long)st.st_mtime : 0;
    char name[96];
    snprintf(name, sizeof(name), "%016lx_%ld_cover.bmp", hash_string(t->path), mtime);
    if (!path_join(bmp_path, path_len, dir, name))
        return false;

    if (access(bmp_path, R_OK) == 0) return true;
    char none_path[PATH_MAX];
    char none_name[96];
    snprintf(none_name, sizeof(none_name), "%016lx_%ld_cover.none", hash_string(t->path), mtime);
    if (path_join(none_path, sizeof(none_path), dir, none_name) &&
        access(none_path, R_OK) == 0)
        return false;
    if (!command_exists("ffmpeg")) return false;

    pid_t pid = fork();
    if (pid == 0) {
        execlp("ffmpeg", "ffmpeg",
               "-y", "-hide_banner", "-loglevel", "error",
               "-i", t->path,
               "-map", "0:v:0",
               "-frames:v", "1",
               "-vf", "scale=512:512:force_original_aspect_ratio=increase,crop=512:512",
               bmp_path,
               (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return false;

    int status = 0;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && access(bmp_path, R_OK) == 0;
    if (!ok && none_path[0] != '\0') {
        FILE *fp = fopen(none_path, "w");
        if (fp) fclose(fp);
    }
    return ok;
}

static void library_load_next_cover(Library *lib, SDL_Renderer *r)
{
    if (lib->covers_loaded) return;
    while (lib->cover_cursor < lib->count) {
        int i = lib->cover_cursor++;
        if (lib->covers[i]) continue;

        char bmp_path[PATH_MAX];
        if (!make_cover_bmp(&lib->tracks[i], bmp_path, sizeof(bmp_path)))
            continue;

        SDL_Surface *surf = SDL_LoadBMP(bmp_path);
        if (!surf) continue;
        lib->covers[i] = SDL_CreateTextureFromSurface(r, surf);
        SDL_FreeSurface(surf);
        return;
    }
    lib->covers_loaded = true;
}

static double player_position(const Player *p)
{
    if (p->current < 0) return 0.0;
    if (!p->playing) return p->paused_pos;
    double elapsed = (double)(SDL_GetTicks() - p->started_ticks) / 1000.0;
    double pos = p->paused_pos + elapsed;
    if (p->duration_sec > 0 && pos > p->duration_sec)
        pos = p->duration_sec;
    return pos;
}

static void player_seek(Player *p, double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    if (p->duration_sec > 0 && seconds > p->duration_sec)
        seconds = p->duration_sec;
    p->paused_pos = seconds;
    p->started_ticks = SDL_GetTicks();
    if (p->pid > 0) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "{\"command\":[\"set_property\",\"time-pos\",%.3f]}", seconds);
        ipc_send(p, cmd);
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

static void draw_round_rect(SDL_Renderer *r, int x, int y, int w, int h, int radius,
                            Uint8 rr, Uint8 g, Uint8 b, Uint8 a)
{
    if (radius <= 0 || w <= radius * 2 || h <= radius * 2) {
        draw_rect(r, x, y, w, h, rr, g, b, a);
        return;
    }
    draw_rect(r, x + radius, y, w - radius * 2, h, rr, g, b, a);
    draw_rect(r, x, y + radius, w, h - radius * 2, rr, g, b, a);
    for (int dy = 0; dy < radius; dy++) {
        int dx = radius;
        while (dx > 0 && dx * dx + dy * dy > radius * radius) dx--;
        draw_rect(r, x + radius - dx, y + radius - dy - 1, dx * 2 + w - radius * 2, 1, rr, g, b, a);
        draw_rect(r, x + radius - dx, y + h - radius + dy, dx * 2 + w - radius * 2, 1, rr, g, b, a);
    }
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
                                int y, bool active, bool hovered)
{
    if (active)
        draw_round_rect(r, 16, y, SIDEBAR_W - 32, 38, 8, 24, 42, 69, 255);
    else if (hovered)
        draw_round_rect(r, 16, y, SIDEBAR_W - 32, 38, 8, 32, 36, 47, 255);
    draw_text_fit(r, ui->md, label, 30, y + 10, SIDEBAR_W - 60,
                  active ? 127 : hovered ? 210 : 136,
                  active ? 176 : hovered ? 214 : 136,
                  active ? 255 : hovered ? 222 : 146);
}

static int context_menu_row_at(const Library *lib, int mx, int my)
{
    if (!lib->menu_open || lib->menu_track < 0 || lib->menu_track >= lib->count)
        return -1;
    int rows = 3 + lib->playlist_count;
    int w = 260;
    int h = rows * 30 + 14;
    int x = lib->menu_x;
    int y = lib->menu_y;
    if (x + w > win_w - 8) x = win_w - w - 8;
    if (y + h > win_h - PLAYER_H - 8) y = win_h - PLAYER_H - h - 8;
    if (mx < x || mx >= x + w || my < y + 8 || my >= y + h - 6)
        return -1;
    return (my - (y + 8)) / 30;
}

static void art_color(const char *seed, Uint8 *r, Uint8 *g, Uint8 *b)
{
    unsigned hash = 2166136261u;
    for (const char *p = seed ? seed : ""; *p; p++) {
        hash ^= (unsigned char)*p;
        hash *= 16777619u;
    }
    const Uint8 palette[][3] = {
        {26, 51, 85}, {26, 64, 48}, {53, 26, 68},
        {63, 42, 18}, {22, 48, 74}, {42, 26, 63}
    };
    const Uint8 *c = palette[hash % 6];
    *r = c[0]; *g = c[1]; *b = c[2];
}

static void draw_art(SDL_Renderer *r, const UI *ui, const Track *t,
                     SDL_Texture *cover, int x, int y, int size)
{
    if (cover) {
        SDL_Rect dst = {x, y, size, size};
        SDL_RenderCopy(r, cover, NULL, &dst);
        draw_rect(r, x, y, size, 1, 70, 78, 94, 180);
        draw_rect(r, x, y + size - 1, size, 1, 8, 9, 12, 180);
        return;
    }

    Uint8 rr, gg, bb;
    art_color(t ? t->album[0] ? t->album : t->title : "ctify", &rr, &gg, &bb);
    draw_rect(r, x, y, size, size, rr, gg, bb, 255);
    draw_rect(r, x, y, size, 1, 70, 78, 94, 255);
    draw_rect(r, x, y + size - 1, size, 1, 8, 9, 12, 255);
    char letter[8] = "♪";
    if (t && t->title[0]) {
        letter[0] = (char)toupper((unsigned char)t->title[0]);
        letter[1] = '\0';
    }
    draw_text(r, size > 80 ? ui->lg : ui->md, letter,
              x + size / 2 - 9, y + size / 2 - 14, 245, 247, 250);
}

static void draw_home_cards(SDL_Renderer *r, const UI *ui, Library *lib,
                            int content_x, int content_w)
{
    int x0 = content_x + LEFT_PAD;
    int card = 148;
    int art = 132;
    int gap = 18;
    int cols = (content_w - LEFT_PAD * 2 + gap) / (card + gap);
    if (cols < 1) cols = 1;

    int y = TOP_H + 28;
    draw_text(r, ui->sm, "FOLDERS", x0, y, 101, 106, 116);
    y += 30;
    for (int i = 0; i < lib->folder_count; i++) {
        int row = i / cols, col = i % cols;
        int x = x0 + col * (card + gap);
        int cy = y + row * 206;
        draw_rect(r, x - 8, cy - 8, card, 190, 20, 23, 30, 255);
        Track fake = {0};
        copy_text(fake.title, sizeof(fake.title), basename_of(lib->folders[i]));
        draw_art(r, ui, &fake, NULL, x, cy, art);
        draw_text_fit(r, ui->md, basename_of(lib->folders[i]), x, cy + art + 12, art, 224, 224, 224);
        char sub[64];
        int total = folder_track_count(lib, lib->folders[i]);
        int favs = folder_favorite_count(lib, lib->folders[i]);
        if (favs > 0)
            snprintf(sub, sizeof(sub), "%d songs, %d fav", total, favs);
        else
            snprintf(sub, sizeof(sub), "%d songs", total);
        draw_text_fit(r, ui->sm, sub, x, cy + art + 34, art, 116, 121, 132);
    }

    y += ((lib->folder_count + cols - 1) / cols) * 206 + 36;
    draw_text(r, ui->sm, "ARTISTS", x0, y, 101, 106, 116);
    y += 30;
    int a = 0;
    for (int i = 0; i < lib->count; i++) {
        if (!lib->tracks[i].artist[0] || artist_seen_before(lib, i)) continue;
        int row = a / cols, col = a % cols;
        int x = x0 + col * (card + gap);
        int cy = y + row * 206;
        draw_rect(r, x - 8, cy - 8, card, 190, 20, 23, 30, 255);
        draw_art(r, ui, &lib->tracks[i], lib->covers[i], x, cy, art);
        draw_text_fit(r, ui->md, lib->tracks[i].artist, x, cy + art + 12, art, 224, 224, 224);
        char sub[64];
        snprintf(sub, sizeof(sub), "%d songs", artist_track_count(lib, lib->tracks[i].artist));
        draw_text_fit(r, ui->sm, sub, x, cy + art + 34, art, 116, 121, 132);
        a++;
        if (a >= 24) break;
    }
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
    draw_sidebar_button(r, ui, "Home", 94, lib->view == VIEW_HOME,
                        lib->hover_nav == VIEW_HOME);
    draw_sidebar_button(r, ui, "All Songs", 138, lib->view == VIEW_ALL,
                        lib->hover_nav == VIEW_ALL);
    draw_sidebar_button(r, ui, "Add Music", 182, false,
                        lib->hover_nav == NAV_ADD_MUSIC);
    draw_sidebar_button(r, ui, "Refresh", 226, false,
                        lib->hover_nav == NAV_REFRESH);
    draw_rect(r, 20, 270, SIDEBAR_W - 40, 1, 42, 44, 51, 255);
    draw_text(r, ui->sm, "PLAYLISTS", 28, 282, 96, 100, 109);
    draw_round_rect(r, SIDEBAR_W - 54, 274, 30, 26, 7,
                    lib->hover_nav == NAV_NEW_PLAYLIST ? 45 : 31,
                    lib->hover_nav == NAV_NEW_PLAYLIST ? 51 : 36,
                    lib->hover_nav == NAV_NEW_PLAYLIST ? 66 : 47, 255);
    draw_text(r, ui->md, "+", SIDEBAR_W - 44, 278, 210, 214, 222);
    draw_sidebar_button(r, ui, "Favorites", 310, lib->view == VIEW_FAVORITES,
                        lib->hover_nav == VIEW_FAVORITES);
    for (int i = 0; i < lib->playlist_count; i++) {
        int y = 354 + i * 40;
        if (y + 36 >= win_h - PLAYER_H) break;
        bool active = lib->view == VIEW_PLAYLIST && lib->active_playlist == i;
        draw_sidebar_button(r, ui, lib->playlists[i].name, y, active,
                            lib->hover_nav == VIEW_PLAYLIST && lib->hover_playlist == i);
    }

    draw_rect(r, content_x, 0, content_w, win_h - PLAYER_H, 16, 17, 22, 255);
    draw_rect(r, content_x, 0, content_w, TOP_H, 16, 17, 22, 255);
    const char *view_title = lib->view == VIEW_HOME ? "Home" :
                             lib->view == VIEW_FAVORITES ? "Favorites" :
                             lib->view == VIEW_FOLDER ? basename_of(lib->filter) :
                             lib->view == VIEW_ARTIST ? lib->filter :
                             lib->view == VIEW_ALBUM ? lib->filter :
                             lib->view == VIEW_PLAYLIST && lib->active_playlist >= 0 ?
                             lib->playlists[lib->active_playlist].name : "All Songs";
    draw_text(r, ui->lg, view_title, content_x + LEFT_PAD, 24, 240, 240, 240);

    char info[256];
    snprintf(info, sizeof(info), "%d songs  |  %d favorites  |  / search",
             lib->count, favorite_count(lib));
    draw_text_fit(r, ui->sm, info, content_x + LEFT_PAD + 150, 34,
                  content_w - 180, 96, 100, 109);

    draw_rect(r, content_x, TOP_H - 1, content_w, 1, 42, 44, 51, 255);

    if (lib->view == VIEW_HOME && lib->search[0] == '\0') {
        SDL_Rect clip_home = {content_x, TOP_H, content_w, win_h - TOP_H - PLAYER_H};
        SDL_RenderSetClipRect(r, &clip_home);
        draw_home_cards(r, ui, lib, content_x, content_w);
        SDL_RenderSetClipRect(r, NULL);
        goto draw_now_panel;
    }

    bool show_bpm = content_w >= 720;
    bool show_artist = content_w >= 620;
    bool show_time = content_w >= 470;
    int title_x = content_x + LEFT_PAD + 90;
    int right_edge = content_x + content_w - 28;
    int bpm_x = right_edge - 50;
    int time_x = show_bpm ? bpm_x - 66 : right_edge - 54;
    int artist_x = show_time ? time_x - 220 : right_edge - 190;
    int title_right = show_artist ? artist_x - 18 :
                      show_time ? time_x - 18 : right_edge;
    int title_w = title_right - title_x;
    if (title_w < 120) title_w = right_edge - title_x;
    if (title_w < 80) title_w = 80;

    draw_text(r, ui->sm, "#", content_x + LEFT_PAD, TOP_H + 12, 68, 68, 74);
    draw_text(r, ui->sm, "TITLE", content_x + LEFT_PAD + 52, TOP_H + 12, 68, 68, 74);
    if (show_artist)
        draw_text(r, ui->sm, "ARTIST", artist_x, TOP_H + 12, 68, 68, 74);
    if (show_time)
        draw_text(r, ui->sm, "TIME", time_x, TOP_H + 12, 68, 68, 74);
    if (show_bpm)
        draw_text(r, ui->sm, "BPM", bpm_x, TOP_H + 12, 68, 68, 74);

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
        bool hovered = i == lib->hovered && !selected;
        bool current = i == player->current;
        if (hovered)
            draw_rect(r, content_x + 18, y + 3, content_w - 36, ROW_H - 6, 28, 32, 42, 255);
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
        draw_art(r, ui, t, lib->covers[i], content_x + LEFT_PAD + 48, y + 7, 32);
        draw_text_fit(r, ui->md, t->title, title_x, y + 8,
                      title_w, current ? 79 : 224, current ? 142 : 224, current ? 247 : 224);
        const char *detail = t->album[0] ? t->album : t->folder;
        if (!show_artist && t->artist[0])
            detail = t->artist;
        draw_text_fit(r, ui->sm, detail, title_x, y + 27, title_w, 96, 100, 109);
        if (show_artist)
            draw_text_fit(r, ui->sm, t->artist, artist_x, y + 14,
                          190, 90, 143, 200);
        char dur[32];
        format_time(t->duration_sec, dur, sizeof(dur));
        if (show_time)
            draw_text_fit(r, ui->sm, dur, time_x, y + 14,
                          54, 96, 100, 109);
        if (show_bpm) {
            char bpm[32];
            snprintf(bpm, sizeof(bpm), t->bpm > 0.0 ? "%.0f" : "-", t->bpm);
            draw_text_fit(r, ui->sm, bpm, bpm_x, y + 14,
                          50, 96, 100, 109);
        }
    }

    SDL_RenderSetClipRect(r, NULL);

    if (lib->count == 0) {
        draw_text(r, ui->md, "No music found. Put audio files in ~/Music or run ctify /path/to/music.",
                  content_x + LEFT_PAD, table_top + 44, 174, 180, 190);
    } else if (total_visible == 0) {
        draw_text(r, ui->md, "No tracks match the search.",
                  content_x + LEFT_PAD, table_top + 44, 174, 180, 190);
    }

draw_now_panel:
    if (now_x < win_w) {
        draw_rect(r, now_x, 0, win_w - now_x, win_h - PLAYER_H, 19, 21, 27, 255);
        draw_rect(r, now_x, 0, 2, win_h - PLAYER_H, 47, 53, 64, 255);
        draw_text(r, ui->sm, "NOW PLAYING", now_x + 24, 28, 96, 100, 109);
        const Track *ct = player->current >= 0 && player->current < lib->count ? &lib->tracks[player->current] : NULL;
        SDL_Texture *cover = player->current >= 0 && player->current < lib->count ? lib->covers[player->current] : NULL;
        draw_art(r, ui, ct, cover, now_x + 28, 70, NOW_W - 56);
        if (ct) {
            draw_text_fit(r, ui->md, ct->title, now_x + 24, 292, NOW_W - 48, 240, 240, 240);
            draw_text_fit(r, ui->sm, ct->artist, now_x + 24, 318, NOW_W - 48, 90, 143, 200);
            draw_text_fit(r, ui->sm, ct->album, now_x + 24, 340, NOW_W - 48, 85, 106, 128);
        }
    }

    draw_rect(r, 0, win_h - PLAYER_H, win_w, PLAYER_H, 27, 30, 39, 255);
    draw_rect(r, 0, win_h - PLAYER_H, win_w, 2, 61, 69, 84, 255);

    const char *title = "Nothing playing";
    const char *artist = "";
    if (player->current >= 0 && player->current < lib->count) {
        title = lib->tracks[player->current].title;
        artist = lib->tracks[player->current].artist;
    }

    draw_text_fit(r, ui->md, title, 28, win_h - PLAYER_H + 28,
                  230, 240, 240, 240);
    draw_text_fit(r, ui->sm, artist, 28, win_h - PLAYER_H + 56,
                  230, 102, 102, 102);

    int cx = win_w / 2;
    draw_round_rect(r, cx - 190, win_h - PLAYER_H + 18, 48, 44, 12,
                    lib->hover_control == CTRL_SHUFFLE ? 38 : 27,
                    lib->hover_control == CTRL_SHUFFLE ? 43 : 30,
                    lib->hover_control == CTRL_SHUFFLE ? 54 : 39, 255);
    draw_text(r, ui->md, player->shuffle ? "SHUF" : "shuf", cx - 180, win_h - PLAYER_H + 28,
              player->shuffle ? 79 : 160, player->shuffle ? 142 : 160, player->shuffle ? 247 : 160);

    draw_round_rect(r, cx - 108, win_h - PLAYER_H + 18, 48, 44, 12,
                    lib->hover_control == CTRL_PREV ? 38 : 27,
                    lib->hover_control == CTRL_PREV ? 43 : 30,
                    lib->hover_control == CTRL_PREV ? 54 : 39, 255);
    draw_text(r, ui->md, "<<", cx - 92, win_h - PLAYER_H + 28, 180, 180, 185);

    draw_round_rect(r, cx - 30, win_h - PLAYER_H + 18, 60, 44, 14,
                    lib->hover_control == CTRL_PLAY ? 103 : 79,
                    lib->hover_control == CTRL_PLAY ? 163 : 142,
                    lib->hover_control == CTRL_PLAY ? 255 : 247, 255);
    draw_text(r, ui->md, player->playing ? "II" : ">", cx - 6, win_h - PLAYER_H + 31, 6, 17, 33);

    draw_round_rect(r, cx + 58, win_h - PLAYER_H + 18, 48, 44, 12,
                    lib->hover_control == CTRL_NEXT ? 38 : 27,
                    lib->hover_control == CTRL_NEXT ? 43 : 30,
                    lib->hover_control == CTRL_NEXT ? 54 : 39, 255);
    draw_text(r, ui->md, ">>", cx + 70, win_h - PLAYER_H + 28, 180, 180, 185);

    draw_round_rect(r, cx + 140, win_h - PLAYER_H + 18, 48, 44, 12,
                    lib->hover_control == CTRL_REPEAT ? 38 : 27,
                    lib->hover_control == CTRL_REPEAT ? 43 : 30,
                    lib->hover_control == CTRL_REPEAT ? 54 : 39, 255);
    draw_text(r, ui->md, player->repeat ? "REP" : "rep", cx + 150, win_h - PLAYER_H + 28,
              player->repeat ? 79 : 160, player->repeat ? 142 : 160, player->repeat ? 247 : 160);

    double pos_sec = player_position(player);
    double frac = player->duration_sec > 0 ? pos_sec / (double)player->duration_sec : 0.0;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    draw_rect(r, cx - 220, win_h - PLAYER_H + 78, 440, 4, 48, 51, 58, 255);
    draw_rect(r, cx - 220, win_h - PLAYER_H + 78, (int)(440.0 * frac), 4, 232, 237, 245, 255);
    draw_rect(r, cx - 222 + (int)(440.0 * frac), win_h - PLAYER_H + 73, 8, 14, 247, 251, 255, 255);
    char vol[64];
    snprintf(vol, sizeof(vol), "Volume %d%%", player->volume);
    draw_text_fit(r, ui->sm, vol, win_w - 150, win_h - PLAYER_H + 58, 120, 96, 100, 109);
    draw_rect(r, win_w - 150, win_h - PLAYER_H + 82, 110, 4, 48, 51, 58, 255);
    draw_rect(r, win_w - 150, win_h - PLAYER_H + 82, player->volume * 110 / 100, 4, 232, 237, 245, 255);

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

    if (lib->menu_open && lib->menu_track >= 0 && lib->menu_track < lib->count) {
        int rows = 3 + lib->playlist_count;
        int w = 260;
        int h = rows * 30 + 14;
        int x = lib->menu_x;
        int y = lib->menu_y;
        if (x + w > win_w - 8) x = win_w - w - 8;
        if (y + h > win_h - PLAYER_H - 8) y = win_h - PLAYER_H - h - 8;
        draw_rect(r, x - 3, y - 3, w + 6, h + 6, 0, 0, 0, 150);
        draw_round_rect(r, x, y, w, h, 8, 28, 31, 40, 255);
        int row_y = y + 8;
        const Track *mt = &lib->tracks[lib->menu_track];
        if (lib->menu_hover == 0)
            draw_round_rect(r, x + 6, row_y, w - 12, 28, 6, 43, 49, 62, 255);
        draw_text_fit(r, ui->sm, mt->favorite ? "Unlike song" : "Like song",
                      x + 14, row_y + 7, w - 28, 232, 235, 240);
        row_y += 30;
        if (lib->menu_hover == 1)
            draw_round_rect(r, x + 6, row_y, w - 12, 28, 6, 43, 49, 62, 255);
        draw_text_fit(r, ui->sm, "Add to Library playlist",
                      x + 14, row_y + 7, w - 28, 190, 198, 210);
        row_y += 30;
        for (int i = 0; i < lib->playlist_count; i++) {
            char label[TITLE_LEN + 16];
            snprintf(label, sizeof(label), "Add to %s", lib->playlists[i].name);
            if (lib->menu_hover == i + 2)
                draw_round_rect(r, x + 6, row_y, w - 12, 28, 6, 43, 49, 62, 255);
            draw_text_fit(r, ui->sm, label, x + 14, row_y + 7, w - 28, 190, 198, 210);
            row_y += 30;
        }
        draw_rect(r, x + 10, row_y + 2, w - 20, 1, 55, 60, 72, 255);
        if (lib->menu_hover == 2 + lib->playlist_count)
            draw_round_rect(r, x + 6, row_y + 4, w - 12, 28, 6, 65, 39, 45, 255);
        draw_text_fit(r, ui->sm,
                      lib->view == VIEW_PLAYLIST ? "Remove from playlist" : "Remove from library",
                      x + 14, row_y + 8, w - 28, 230, 126, 126);
    }

    if (lib->playlist_name_active) {
        int w = 480, h = 96;
        int x = (win_w - w) / 2;
        int y = (win_h - h) / 2;
        draw_rect(r, 0, 0, win_w, win_h, 0, 0, 0, 120);
        draw_round_rect(r, x, y, w, h, 10, 28, 31, 40, 255);
        draw_text(r, ui->md, "New playlist", x + 18, y + 16, 238, 238, 240);
        draw_round_rect(r, x + 18, y + 48, w - 36, 32, 7, 18, 20, 27, 255);
        char q[TITLE_LEN + 8];
        snprintf(q, sizeof(q), "%s|", lib->playlist_name);
        draw_text_fit(r, ui->md, q, x + 30, y + 56, w - 60, 238, 238, 240);
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

static void playback_state_file(char *out, size_t out_len)
{
    char state_dir[PATH_MAX];
    ensure_state_dir(state_dir, sizeof(state_dir));
    path_join(out, out_len, state_dir, "playback_state.txt");
}

static void save_playback_state(const Player *player, const Library *lib)
{
    char path[PATH_MAX];
    playback_state_file(path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    const char *track = "";
    if (player->current >= 0 && player->current < lib->count)
        track = lib->tracks[player->current].path;
    fprintf(fp, "track=%s\nposition=%.0f\nshuffle=%d\nrepeat=%d\nvolume=%d\n",
            track, player_position(player), player->shuffle ? 1 : 0,
            player->repeat ? 1 : 0, player->volume);
    fclose(fp);
}

static void restore_playback_state(Player *player, Library *lib)
{
    char path[PATH_MAX];
    playback_state_file(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char track[PATH_MAX] = "";
    double pos = 0.0;
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof(line), fp)) {
        trim_line(line);
        if (strncmp(line, "track=", 6) == 0) copy_text(track, sizeof(track), line + 6);
        else if (strncmp(line, "position=", 9) == 0) pos = atof(line + 9);
        else if (strncmp(line, "shuffle=", 8) == 0) player->shuffle = atoi(line + 8) != 0;
        else if (strncmp(line, "repeat=", 7) == 0) player->repeat = atoi(line + 7) != 0;
        else if (strncmp(line, "volume=", 7) == 0) player->volume = atoi(line + 7);
    }
    fclose(fp);

    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->tracks[i].path, track) != 0) continue;
        lib->selected = i;
        player_play(player, lib, i);
        if (pos > 0.0) {
            SDL_Delay(250);
            player_seek(player, pos);
        }
        SDL_Delay(100);
        player_pause_toggle(player);
        break;
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

static void toggle_visible_favorites(Library *lib)
{
    bool make_favorite = false;
    for (int i = 0; i < lib->count; i++) {
        if (matches_search(lib, i) && !lib->tracks[i].favorite) {
            make_favorite = true;
            break;
        }
    }
    for (int i = 0; i < lib->count; i++)
        if (matches_search(lib, i))
            lib->tracks[i].favorite = make_favorite;
    save_favorites(lib);
    if (lib->view == VIEW_FAVORITES && !make_favorite)
        select_first_visible(lib);
}

static void toggle_folder_favorites(Library *lib, const char *folder)
{
    bool make_favorite = false;
    size_t n = strlen(folder);
    for (int i = 0; i < lib->count; i++) {
        bool in_folder = strncmp(lib->tracks[i].path, folder, n) == 0 &&
                         (lib->tracks[i].path[n] == '/' || lib->tracks[i].path[n] == '\0');
        if (in_folder && !lib->tracks[i].favorite) {
            make_favorite = true;
            break;
        }
    }
    for (int i = 0; i < lib->count; i++) {
        bool in_folder = strncmp(lib->tracks[i].path, folder, n) == 0 &&
                         (lib->tracks[i].path[n] == '/' || lib->tracks[i].path[n] == '\0');
        if (in_folder)
            lib->tracks[i].favorite = make_favorite;
    }
    save_favorites(lib);
}

static void remove_selected_from_library(Library *lib, Player *player)
{
    if (lib->selected < 0 || lib->selected >= lib->count) return;
    char removed[PATH_MAX];
    copy_text(removed, sizeof(removed), lib->tracks[lib->selected].path);
    if (player->current == lib->selected)
        player_stop(player);
    db_remove_track(removed);
    if (lib->covers[lib->selected]) {
        SDL_DestroyTexture(lib->covers[lib->selected]);
        lib->covers[lib->selected] = NULL;
    }
    for (int i = lib->selected; i + 1 < lib->count; i++) {
        lib->tracks[i] = lib->tracks[i + 1];
        lib->covers[i] = lib->covers[i + 1];
    }
    lib->count--;
    lib->covers[lib->count] = NULL;
    if (player->current > lib->selected)
        player->current--;
    if (lib->selected >= lib->count)
        lib->selected = lib->count - 1;
    select_first_visible(lib);
}

static void player_chapter(Player *player, int dir)
{
    if (player->pid <= 0) return;
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "{\"command\":[\"add\",\"chapter\",%d]}", dir);
    ipc_send(player, cmd);
}

static bool set_progress_from_mouse(Player *player, int mx, int my)
{
    int cx = win_w / 2;
    int x = cx - 220;
    int y = win_h - PLAYER_H + 66;
    if (mx < x || mx > x + 440 || my < y || my > y + 28)
        return false;
    if (player->duration_sec <= 0) return true;
    double frac = (double)(mx - x) / 440.0;
    player_seek(player, frac * player->duration_sec);
    return true;
}

static bool set_volume_from_mouse(Player *player, int mx, int my)
{
    int x = win_w - 150;
    int y = win_h - PLAYER_H + 70;
    if (mx < x || mx > x + 110 || my < y || my > y + 28)
        return false;
    int volume = (mx - x) * 100 / 110;
    player_set_volume(player, volume);
    return true;
}

static int player_control_hit(int mx, int my)
{
    int cx = win_w / 2;
    int y = win_h - PLAYER_H + 18;
    if (my < y || my > y + 44) return CTRL_NONE;
    if (mx >= cx - 190 && mx <= cx - 142) return CTRL_SHUFFLE;
    if (mx >= cx - 108 && mx <= cx - 60) return CTRL_PREV;
    if (mx >= cx - 30 && mx <= cx + 30) return CTRL_PLAY;
    if (mx >= cx + 58 && mx <= cx + 106) return CTRL_NEXT;
    if (mx >= cx + 140 && mx <= cx + 188) return CTRL_REPEAT;
    return CTRL_NONE;
}

static void activate_player_control(int ctrl, Player *player, Library *lib)
{
    if (ctrl == CTRL_SHUFFLE) {
        player->shuffle = !player->shuffle;
    } else if (ctrl == CTRL_PREV) {
        play_relative(player, lib, -1);
    } else if (ctrl == CTRL_PLAY) {
        if (player->pid > 0)
            player_pause_toggle(player);
        else if (player->current >= 0)
            player_play(player, lib, player->current);
        else if (lib->selected >= 0)
            player_play(player, lib, lib->selected);
    } else if (ctrl == CTRL_NEXT) {
        play_relative(player, lib, 1);
    } else if (ctrl == CTRL_REPEAT) {
        player->repeat = !player->repeat;
    }
}

static void open_playlist_name(Library *lib)
{
    lib->playlist_name_active = true;
    lib->menu_open = false;
    lib->playlist_name[0] = '\0';
    SDL_StartTextInput();
}

static void commit_playlist_name(Library *lib)
{
    trim_line(lib->playlist_name);
    int pl = create_playlist_named(lib, lib->playlist_name[0] ? lib->playlist_name : "Playlist");
    lib->playlist_name_active = false;
    lib->playlist_name[0] = '\0';
    if (pl >= 0) {
        lib->view = VIEW_PLAYLIST;
        lib->active_playlist = pl;
        lib->filter[0] = '\0';
        lib->scroll_y = 0;
        select_first_visible(lib);
    }
}

static void activate_context_menu_row(Library *lib, Player *player, int mx, int my)
{
    if (!lib->menu_open || lib->menu_track < 0 || lib->menu_track >= lib->count)
        return;
    int row = context_menu_row_at(lib, mx, my);
    if (row < 0) {
        lib->menu_open = false;
        return;
    }
    int track = lib->menu_track;
    lib->selected = track;
    if (row == 0) {
        lib->tracks[track].favorite = !lib->tracks[track].favorite;
        save_favorites(lib);
    } else if (row == 1) {
        add_to_playlist(lib, ensure_default_playlist(lib), track);
    } else if (row >= 2 && row < 2 + lib->playlist_count) {
        add_to_playlist(lib, row - 2, track);
    } else {
        if (lib->view == VIEW_PLAYLIST && lib->active_playlist >= 0)
            remove_from_playlist(lib, lib->active_playlist, track);
        else
            remove_selected_from_library(lib, player);
    }
    lib->menu_open = false;
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
    for (int i = 1; i < argc; i++)
        db_add_folder(argv[i]);
    library_scan(lib);

    Player player = {.pid = -1, .current = -1, .playing = false, .volume = 70};
    restore_playback_state(&player, lib);
    SDL_StartTextInput();

    bool running = true;
    bool dragging_progress = false;
    bool dragging_volume = false;
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
                if (lib->menu_open) {
                    activate_context_menu_row(lib, &player, ev.button.x, ev.button.y);
                    continue;
                }
                if (lib->playlist_name_active)
                    continue;
                if (new_playlist_hit(ev.button.x, ev.button.y)) {
                    open_playlist_name(lib);
                    continue;
                }
                int ctrl = player_control_hit(ev.button.x, ev.button.y);
                if (ctrl != CTRL_NONE) {
                    activate_player_control(ctrl, &player, lib);
                    lib->hover_control = ctrl;
                    continue;
                }
                if (set_progress_from_mouse(&player, ev.button.x, ev.button.y)) {
                    dragging_progress = true;
                    continue;
                }
                if (set_volume_from_mouse(&player, ev.button.x, ev.button.y)) {
                    dragging_volume = true;
                    continue;
                }
                int playlist_idx = -1;
                int nav = sidebar_hit(lib, ev.button.x, ev.button.y, &playlist_idx);
                if (nav == NAV_ADD_MUSIC) {
                    import_audio_files_with_picker(lib);
                } else if (nav == NAV_REFRESH) {
                    library_scan(lib);
                } else if (nav >= 0) {
                    lib->view = (ViewMode)nav;
                    lib->active_playlist = playlist_idx;
                    lib->filter[0] = '\0';
                    lib->scroll_y = 0;
                    select_first_visible(lib);
                } else {
                    char filter[TITLE_LEN];
                    int home_nav = lib->view == VIEW_HOME ?
                        home_hit(lib, ev.button.x, ev.button.y, filter, sizeof(filter)) : -1;
                    if (home_nav >= 0) {
                        lib->view = (ViewMode)home_nav;
                        lib->active_playlist = -1;
                        copy_text(lib->filter, sizeof(lib->filter), filter);
                        lib->scroll_y = 0;
                        select_first_visible(lib);
                        continue;
                    }
                    int idx = hit_test(lib, ev.button.x, ev.button.y);
                    if (idx >= 0) {
                        lib->selected = idx;
                        lib->hovered = idx;
                        ensure_selected_visible(lib);
                        player_play(&player, lib, idx);
                    }
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_RIGHT) {
                if (lib->playlist_name_active)
                    continue;
                int playlist_idx = -1;
                int nav = sidebar_hit(lib, ev.button.x, ev.button.y, &playlist_idx);
                if (nav == VIEW_PLAYLIST && playlist_idx >= 0) {
                    delete_playlist(lib, playlist_idx);
                    continue;
                }
                char filter[TITLE_LEN];
                int home_nav = lib->view == VIEW_HOME ?
                    home_hit(lib, ev.button.x, ev.button.y, filter, sizeof(filter)) : -1;
                if (home_nav == VIEW_FOLDER) {
                    toggle_folder_favorites(lib, filter);
                    continue;
                }
                int idx = hit_test(lib, ev.button.x, ev.button.y);
                if (idx >= 0) {
                    lib->menu_open = true;
                    lib->menu_x = ev.button.x;
                    lib->menu_y = ev.button.y;
                    lib->menu_track = idx;
                    lib->menu_hover = -1;
                    lib->selected = idx;
                }
            } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
                dragging_progress = false;
                dragging_volume = false;
            } else if (ev.type == SDL_MOUSEMOTION) {
                if (dragging_progress)
                    set_progress_from_mouse(&player, ev.motion.x, ev.motion.y);
                if (dragging_volume)
                    set_volume_from_mouse(&player, ev.motion.x, ev.motion.y);
                if (!dragging_progress && !dragging_volume) {
                    if (lib->menu_open) {
                        lib->menu_hover = context_menu_row_at(lib, ev.motion.x, ev.motion.y);
                        lib->hover_nav = -1;
                        lib->hover_playlist = -1;
                        lib->hover_control = CTRL_NONE;
                        lib->hovered = -1;
                        continue;
                    }
                    int playlist_idx = -1;
                    lib->hover_nav = sidebar_hit(lib, ev.motion.x, ev.motion.y, &playlist_idx);
                    if (new_playlist_hit(ev.motion.x, ev.motion.y))
                        lib->hover_nav = NAV_NEW_PLAYLIST;
                    lib->hover_playlist = playlist_idx;
                    lib->hover_control = player_control_hit(ev.motion.x, ev.motion.y);
                    lib->hovered = hit_test(lib, ev.motion.x, ev.motion.y);
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
            } else if (ev.type == SDL_TEXTINPUT && lib->playlist_name_active) {
                size_t len = strlen(lib->playlist_name);
                if (len + strlen(ev.text.text) < sizeof(lib->playlist_name))
                    strcat(lib->playlist_name, ev.text.text);
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode key = ev.key.keysym.sym;
                bool shift = (ev.key.keysym.mod & KMOD_SHIFT) != 0;
                if (lib->playlist_name_active) {
                    if (key == SDLK_ESCAPE) {
                        lib->playlist_name_active = false;
                        lib->playlist_name[0] = '\0';
                    } else if (key == SDLK_BACKSPACE) {
                        size_t len = strlen(lib->playlist_name);
                        if (len > 0) lib->playlist_name[len - 1] = '\0';
                    } else if (key == SDLK_RETURN) {
                        commit_playlist_name(lib);
                    }
                } else if (lib->search_active) {
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
                    if (lib->menu_open)
                        lib->menu_open = false;
                    else
                        running = false;
                } else if (key == SDLK_SLASH) {
                    lib->search_active = true;
                    lib->search[0] = '\0';
                    lib->ignore_next_text = true;
                } else if (key == SDLK_r) {
                    library_scan(lib);
                } else if (key == SDLK_o) {
                    import_audio_files_with_picker(lib);
                } else if (key == SDLK_SPACE) {
                    if (player.pid > 0)
                        player_pause_toggle(&player);
                    else if (player.current >= 0)
                        player_play(&player, lib, player.current);
                    else if (lib->selected >= 0)
                        player_play(&player, lib, lib->selected);
                } else if (key == SDLK_RETURN) {
                    play_selected(&player, lib);
                } else if (key == SDLK_n) {
                    play_relative(&player, lib, 1);
                } else if (key == SDLK_b) {
                    play_relative(&player, lib, -1);
                } else if (key == SDLK_f) {
                    if (shift)
                        toggle_visible_favorites(lib);
                    else
                        toggle_favorite(lib);
                } else if (key == SDLK_a) {
                    int pl = lib->view == VIEW_PLAYLIST ? lib->active_playlist : ensure_default_playlist(lib);
                    if (shift)
                        add_visible_to_playlist(lib, pl);
                    else
                        add_to_playlist(lib, pl, lib->selected);
                } else if (key == SDLK_p) {
                    open_playlist_name(lib);
                } else if (key == SDLK_d) {
                    if (lib->view == VIEW_PLAYLIST && lib->active_playlist >= 0)
                        delete_playlist(lib, lib->active_playlist);
                } else if (key == SDLK_s) {
                    player.shuffle = !player.shuffle;
                } else if (key == SDLK_t) {
                    player.repeat = !player.repeat;
                } else if (key == SDLK_m) {
                    lib->bpm_sort = !lib->bpm_sort;
                    qsort(lib->tracks, (size_t)lib->count, sizeof(Track),
                          lib->bpm_sort ? cmp_track_bpm : cmp_track);
                    select_first_visible(lib);
                } else if (key == SDLK_DELETE) {
                    remove_selected_from_library(lib, &player);
                } else if (key == SDLK_LEFTBRACKET) {
                    player_chapter(&player, -1);
                } else if (key == SDLK_RIGHTBRACKET) {
                    player_chapter(&player, 1);
                } else if (key == SDLK_1) {
                    lib->view = VIEW_HOME;
                    lib->filter[0] = '\0';
                    lib->scroll_y = 0;
                    select_first_visible(lib);
                } else if (key == SDLK_2) {
                    lib->view = VIEW_ALL;
                    lib->filter[0] = '\0';
                    lib->scroll_y = 0;
                    select_first_visible(lib);
                } else if (key == SDLK_3) {
                    lib->view = VIEW_FAVORITES;
                    lib->filter[0] = '\0';
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
        library_load_next_cover(lib, renderer);
    }

    SDL_StopTextInput();
    save_playback_state(&player, lib);
    player_stop(&player);
    library_free_covers(lib);
    free(lib);
    ui_close(&ui);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
