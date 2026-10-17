/* SacredFire.exe (Windows): the launcher, a native Win32 program (the Linux one is GTK4/Python,
 * which cannot be produced from Linux for the bundled Python; this one is built by the cross
 * toolchain with no dependency beyond Windows). GPL-2.0-or-later.
 *
 * Window: the game folder (picker; the game check decides), the game language, online play
 * (Offline, Join server: the host's address, Host server: this PC runs tools\ufc3_server.py for
 * the session, see "Host server" below), the controller for port 1, the game patches (written to bbport.ini as the in-game menu does), PS4 Pro mode, fullscreen,
 * the developer switches (frame statistics, Vulkan validation, clear the shader cache, menu
 * trace), the log folder (empty: <data>\logs; BB_LOG_DIR wins over the window), Play/Stop, the
 * log of the run, and buttons for the logs and data folders. Settings are kept in
 * <data>\sacredfire.ini.
 * Built with -DUFC3_DIAGNOSTIC_PACKAGE (the UFC-42 diagnostic test), the launcher keeps its
 * data beside the exe in diagnostic-data and forces the write windows with their audit
 * (BB_WRITE_STEP=1, BB_WRITE_WINDOW=1, BB_WRITE_WINDOW_DIAG=1, BB_WRITE_LOG=0), no .cmd needed.
 * Play does what run.sh does on Linux: the preparation scripts with the bundled Python
 * (python\python.exe, the official embeddable package; game_check, prepare, link_libc,
 * link_modules, gen_import_names with the bundled aerolib.inl, content_profile, patches),
 * then bin\sacredfire-game.exe with the same arguments and BB_RESTART_COMMAND for the in-game
 * restart. Everything printed goes to the window, to <data>\logs\<time>.log, and to the console
 * when started from one.
 * Command line: `--play` starts the game at once with the saved settings and no window (Steam
 * shortcuts); `--game <folder>` sets the folder; other arguments go to the game.
 * Look (UFC-28): the colours of the UFC 3 retail cover (near-black, gold, UFC red for Play/Stop),
 * owner-drawn buttons and check boxes, sections drawn by the window; the banner shows the game's
 * own sce_sys\icon0.png (or pic1.png) read with WIC from the user's dump: no EA art is shipped.
 * Game resolution 2K / 4K (experimental, UFC-45/46): PS4 Pro with a 3840x2160 VideoOut and the
 * scene patch of patches\experiments\ufc46-1440p / ufc45-4k; in game: VIDEO -> Favor Resolution.
 * Data (generated files, saves, shader cache, bbport.ini, logs): %LOCALAPPDATA%\SacredFire, or
 * BB_DATA_DIR; BB_DATA_DIR=. keeps everything beside the executable. UTF-8 inside (the manifest
 * selects the UTF-8 code page), UTF-16 at the Win32 calls. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
/* Winsock and IP Helper are loaded at run time (net_load): the launcher links neither. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define PATHW (MAX_PATH * 4)
static wchar_t root[PATHW];   /* the folder this program runs from */
static wchar_t data[PATHW];   /* data directory */
static wchar_t python[PATHW], game_exe[PATHW], caps_exe[PATHW];

/* ---- settings (<data>\sacredfire.ini) ---- */
typedef struct {
    wchar_t game[PATHW], gamepad[128], player[32];
    /* Online play: offline, join the server at join_address, or host one at host_address (an
     * IPv4 address of this PC the friend reaches: LAN or VPN). BB_ONLINE_SERVER is the one in use. */
    int online_mode;
    wchar_t join_address[256], host_address[64];
    int neo, fullscreen, clear_cache, validation, frame_stats, menu_trace;
    /* UFC 3, as the GoW3R launcher's Video panel: the PS4 output size, the frame-rate cap,
     * the swapchain present mode; show_fps mirrors bbport.ini. */
    int resolution, fps_mode, present_mode, show_fps, upscaler, sharpness;
    /* The window (presented image) size: the game's 1080p frame scaled to 1440p / 4K. */
    int display;
    int skip_movies; /* the EA intro and the first-start video replaced by a short black clip */
    int language;    /* index into languages[] */
    /* The folder the run logs (and the frame/readback CSVs) go to; empty: <data>\logs.
     * BB_LOG_DIR wins over it. */
    wchar_t log_dir[PATHW];
} Settings;
static Settings settings = {.resolution = 1, .fps_mode = 5, .present_mode = 2, .upscaler = 0, .sharpness = 2, .skip_movies = 1};
/* Choices of the Video panel (index = the saved value). */
static const wchar_t *const resolution_names[] = {L"1280x720", L"1920x1080", L"2560x1440 (2K, experimental)", L"3840x2160 (4K, experimental)"};
/* 2K and 4K (UFC-45/46, validated on Linux): PS4 Pro with a 3840x2160 VideoOut in both, and the
 * experiment's patch (patches\experiments\<dir>, its own patches json) turning the game's
 * 3200x1800 Favor Resolution scene into 2560x1440 / 3840x2160. The scene only grows with
 * VIDEO -> Favor Resolution chosen in the game. */
static const wchar_t *const resolution_w[] = {L"1280", L"1920", L"3840", L"3840"}, *const resolution_h[] = {L"720", L"1080", L"2160", L"2160"};
static const wchar_t *const resolution_experiment[] = {NULL, NULL, L"ufc46-1440p", L"ufc45-4k"};
static const wchar_t *const resolution_experiment_config[] = {NULL, NULL, L"patches-1440p.json", L"patches-4k.json"};
static const wchar_t *const display_names[] = {L"Same as the game", L"1920x1080 (Full HD)", L"2560x1440 (2K)", L"3840x2160 (4K)"};
static const wchar_t *const display_sizes[] = {NULL, L"1920x1080", L"2560x1440", L"3840x2160"};
static const wchar_t *const fps_names[] = {L"60 FPS", L"90 FPS", L"120 FPS", L"144 FPS", L"240 FPS", L"Unlimited"};
static const wchar_t *const fps_limits[] = {L"60", L"90", L"120", L"144", L"240", L"0"};
/* sacredfire.ini before 90 FPS stored the index into {60, 120, 144, 240, unlimited}; the limit
 * itself is saved now (fps_limit=), and an old fps_mode index is mapped onto the new list. */
static const int legacy_fps_index[] = {0, 2, 3, 4, 5};
static int fps_index_of(const wchar_t *limit) {
    for (int i = 0; i < (int)(sizeof(fps_limits) / sizeof(fps_limits[0])); ++i) if (!wcscmp(fps_limits[i], limit)) return i;
    return -1;
}
/* Immediate is the default on Windows: NVIDIA's Mailbox there paced the menu at twice the
 * display refresh (120 on a 60 Hz laptop panel, Omen 16 in discrete mode; Immediate 240). */
static const wchar_t *const present_names[] = {L"Mailbox (capped at 2x refresh on NVIDIA)", L"VSync (FIFO)", L"Immediate (uncapped, may tear)"};
static const wchar_t *const present_modes[] = {L"Mailbox", L"Fifo", L"Immediate"};
/* Upscaling at present (spatial, so it fits any game): FSR 1.0 when the window or screen is
 * larger than the game's output; RCAS sharpening with it (attenuation in stops: lower = sharper). */
static const wchar_t *const upscaler_names[] = {L"Off (bilinear)", L"AMD FSR 1.0"};
static const wchar_t *const sharpness_names[] = {L"Off", L"Low", L"Medium", L"High"};
static const wchar_t *const sharpness_attenuation[] = {L"2000", L"1000", L"500", L"200"};
#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))
/* The PS4 system language (BB_LANGUAGE, the system parameter the game reads): the languages
 * whose text the game ships (samplegame\data\kettle\loc: eng_us, fre_fr, ger_de, ita_it,
 * spa_es, spa_mx, por_br, pol_pl, rus_ru, jpn_jp, chi_hk), and Portuguese (Portugal), which it
 * has no text of. The first is the default; the id is saved (language=). */
static const wchar_t *const language_names[] = {
    L"English (US)", L"French", L"German", L"Italian", L"Spanish (Spain)", L"Spanish (Latin America)",
    L"Portuguese (Brazil)", L"Portuguese (Portugal)", L"Polish", L"Russian", L"Japanese", L"Chinese (Traditional)"};
static const wchar_t *const language_ids[] = {L"1", L"2", L"4", L"5", L"3", L"20", L"17", L"7", L"16", L"8", L"0", L"10"};
static int language_index_of(const wchar_t *id) {
    for (int i = 0; i < COUNT(language_ids); ++i) if (!wcscmp(language_ids[i], id)) return i;
    return -1;
}
/* Variables the launcher sets for the game: their values from the environment the launcher was
 * started with (a value there wins over the window), captured once, because Play sets them in
 * this process and a second Play must not take the first one's values for the user's. */
static const wchar_t *const managed_env[] = {
    L"BB_FULLSCREEN", L"BB_VK_VALIDATION", L"BB_FRAME_STATS", L"BB_MENU_TRACE", L"BB_GAMEPAD", L"BB_NEO",
    L"BB_READBACKS", L"BB_READBACK_LINEAR", L"BB_GPU_WRITE_TWINS", L"BB_WRITE_WINDOW", L"BB_FLIP_RATE", L"BB_VBLANK_HZ",
    L"BB_Z16S8_AS_D16", L"BB_PREUPLOAD", L"BB_WRITE_STEP", L"BB_COPY_GPU_BUFFERS", L"BB_FPS_LIMIT", L"BB_PRESENT_MODE",
    L"BB_INTERNAL_WIDTH", L"BB_INTERNAL_HEIGHT", L"BB_WINDOW_SIZE", L"BB_USER_NAME", L"BB_SKIP_MOVIES", L"BB_SKIP_MOVIE_FILE", L"BB_FSR1", L"BB_RCAS", L"BB_RCAS_ATTENUATION",
    L"BB_LANGUAGE", L"BB_ONLINE_SERVER", L"BB_FRAME_LOG", L"BB_READBACK_LOG", L"BB_PATCHES_DIR", L"BB_PATCHES_CONFIG",
};
static wchar_t managed_value[sizeof(managed_env) / sizeof(managed_env[0])][1024];
static int managed_given[sizeof(managed_env) / sizeof(managed_env[0])];
static void managed_env_capture(void) {
    for (int i = 0; i < (int)(sizeof(managed_env) / sizeof(managed_env[0])); ++i) {
        const DWORD n = GetEnvironmentVariableW(managed_env[i], managed_value[i], 1024);
        managed_given[i] = n && n < 1024;
    }
}
/* The user's value if the launcher was started with one, else `value` (NULL: unset). */
static void game_env(const wchar_t *name, const wchar_t *value) {
    for (int i = 0; i < (int)(sizeof(managed_env) / sizeof(managed_env[0])); ++i)
        if (!wcscmp(managed_env[i], name)) { if (managed_given[i]) value = managed_value[i]; break; }
    SetEnvironmentVariableW(name, value);
}
/* The value the launcher was started with (a .bat, the system), or NULL. */
static const wchar_t *given_env(const wchar_t *name) {
    for (int i = 0; i < (int)(sizeof(managed_env) / sizeof(managed_env[0])); ++i)
        if (!wcscmp(managed_env[i], name)) return managed_given[i] ? managed_value[i] : NULL;
    return NULL;
}
static int clampi(int v, int n) { return v < 0 || v >= n ? 0 : v; }
/* Online play (online_mode= in sacredfire.ini; before it, a non-empty online_server= meant Join). */
enum { ONLINE_OFFLINE, ONLINE_JOIN, ONLINE_HOST };
static const wchar_t *const online_mode_names[] = {L"Offline", L"Join server", L"Host server"};
static const wchar_t *const online_mode_ids[] = {L"offline", L"join", L"host"};

static int exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
static void join(wchar_t *out_path, size_t n, const wchar_t *a, const wchar_t *b) {
    wchar_t joined[PATHW];
    _snwprintf(joined, PATHW, L"%ls\\%ls", a, b); joined[PATHW - 1] = 0;
    wcsncpy(out_path, joined, n - 1); out_path[n - 1] = 0;
}
static void ensure_directory(const wchar_t *path) {
    wchar_t buffer[PATHW];
    wcsncpy(buffer, path, PATHW - 1); buffer[PATHW - 1] = 0;
    for (wchar_t *p = buffer + 3; *p; ++p) if (*p == L'\\') { *p = 0; CreateDirectoryW(buffer, NULL); *p = L'\\'; }
    CreateDirectoryW(buffer, NULL);
}
static void trim(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0; }
static void trimw(wchar_t *s) { size_t n = wcslen(s); while (n && (s[n - 1] == L'\n' || s[n - 1] == L'\r' || s[n - 1] == L' ')) s[--n] = 0; }
static const wchar_t *env_or(const wchar_t *name, const wchar_t *fallback) {
    static wchar_t values[8][1024]; static int next;
    wchar_t *value = values[next++ % 8];
    DWORD n = GetEnvironmentVariableW(name, value, 1024);
    return n && n < 1024 ? value : fallback;
}
/* The folder the logs go to: BB_LOG_DIR when set and not empty, else the window's "Log folder"
 * (settings.log_dir), else <data>\logs; made absolute. A mapped drive or a UNC share
 * (\\server\share) is fine as long as Windows can reach it. */
static void effective_log_dir(wchar_t *out_path) {
    wchar_t chosen[PATHW];
    const wchar_t *env = env_or(L"BB_LOG_DIR", NULL);
    if (env && env[0]) { wcsncpy(chosen, env, PATHW - 1); chosen[PATHW - 1] = 0; }
    else if (settings.log_dir[0]) { wcsncpy(chosen, settings.log_dir, PATHW - 1); chosen[PATHW - 1] = 0; }
    else join(chosen, PATHW, data, L"logs");
    if (!GetFullPathNameW(chosen, PATHW, out_path, NULL)) { wcsncpy(out_path, chosen, PATHW - 1); out_path[PATHW - 1] = 0; }
}
static void read_version(char *version, size_t n) {
    wchar_t path[PATHW]; join(path, PATHW, root, L"VERSION");
    snprintf(version, n, "unknown version");
    FILE *f = _wfopen(path, L"rb");
    if (f) { if (fgets(version, (int)n, f)) trim(version); fclose(f); }
}
static void settings_path(wchar_t *out_path) { join(out_path, PATHW, data, L"sacredfire.ini"); }
static void settings_load(void) {
    wchar_t path[PATHW]; settings_path(path);
    FILE *f = _wfopen(path, L"r, ccs=UTF-8");
    if (!f) return;
    wchar_t line[PATHW];
    int mode_saved = 0;
    while (fgetws(line, PATHW, f)) {
        trimw(line);
        wchar_t *eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0; const wchar_t *value = eq + 1;
        if (!wcscmp(line, L"game")) wcsncpy(settings.game, value, PATHW - 1);
        else if (!wcscmp(line, L"gamepad")) wcsncpy(settings.gamepad, value, 127);
        else if (!wcscmp(line, L"player")) wcsncpy(settings.player, value, 16);
        else if (!wcscmp(line, L"online_server")) wcsncpy(settings.join_address, value, 255); /* Join's address */
        else if (!wcscmp(line, L"host_address")) wcsncpy(settings.host_address, value, 63);
        else if (!wcscmp(line, L"online_mode")) {
            for (int i = 0; i < 3; ++i) if (!wcscmp(online_mode_ids[i], value)) { settings.online_mode = i; mode_saved = 1; }
        }
        else if (!wcscmp(line, L"neo")) settings.neo = _wtoi(value);
        else if (!wcscmp(line, L"fullscreen")) settings.fullscreen = _wtoi(value);
        else if (!wcscmp(line, L"clear_cache")) settings.clear_cache = _wtoi(value);
        else if (!wcscmp(line, L"validation")) settings.validation = _wtoi(value);
        else if (!wcscmp(line, L"frame_stats")) settings.frame_stats = _wtoi(value);
        else if (!wcscmp(line, L"menu_trace")) settings.menu_trace = _wtoi(value);
        else if (!wcscmp(line, L"resolution")) settings.resolution = clampi(_wtoi(value), COUNT(resolution_names));
        else if (!wcscmp(line, L"fps_mode")) settings.fps_mode = legacy_fps_index[clampi(_wtoi(value), 5)];
        else if (!wcscmp(line, L"fps_limit")) { const int i = fps_index_of(value); if (i >= 0) settings.fps_mode = i; }
        else if (!wcscmp(line, L"present_mode")) settings.present_mode = clampi(_wtoi(value), COUNT(present_names));
        else if (!wcscmp(line, L"upscaler")) settings.upscaler = clampi(_wtoi(value), COUNT(upscaler_names));
        else if (!wcscmp(line, L"sharpness")) settings.sharpness = clampi(_wtoi(value), COUNT(sharpness_names));
        else if (!wcscmp(line, L"skip_movies")) settings.skip_movies = _wtoi(value);
        else if (!wcscmp(line, L"log_dir")) wcsncpy(settings.log_dir, value, PATHW - 1);
        else if (!wcscmp(line, L"display")) settings.display = clampi(_wtoi(value), COUNT(display_names));
        else if (!wcscmp(line, L"language")) { const int i = language_index_of(value); if (i >= 0) settings.language = i; }
    }
    fclose(f);
    /* A sacredfire.ini of the launcher with one "Online server" field: an address there was Join. */
    if (!mode_saved) settings.online_mode = settings.join_address[0] ? ONLINE_JOIN : ONLINE_OFFLINE;
}
static void settings_save(void) {
    wchar_t path[PATHW]; settings_path(path);
    FILE *f = _wfopen(path, L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"player=%ls\nonline_mode=%ls\nonline_server=%ls\nhost_address=%ls\n", settings.player,
             online_mode_ids[settings.online_mode], settings.join_address, settings.host_address);
    fwprintf(f, L"game=%ls\ngamepad=%ls\nneo=%d\nfullscreen=%d\nclear_cache=%d\nvalidation=%d\nframe_stats=%d\nmenu_trace=%d\n"
                L"resolution=%d\nfps_limit=%ls\npresent_mode=%d\nupscaler=%d\nsharpness=%d\ndisplay=%d\nskip_movies=%d\nlanguage=%ls\nlog_dir=%ls\n",
             settings.game, settings.gamepad, settings.neo, settings.fullscreen, settings.clear_cache, settings.validation,
             settings.frame_stats, settings.menu_trace, settings.resolution, fps_limits[settings.fps_mode], settings.present_mode,
             settings.upscaler, settings.sharpness, settings.display, settings.skip_movies, language_ids[settings.language], settings.log_dir);
    fclose(f);
}
/* bbport.ini: "key=value" lines (UTF-8), shared with the in-game menu. */
static void config_path(wchar_t *out_path) {
    const wchar_t *c = env_or(L"BB_CONFIG", NULL);
    if (c) { wcsncpy(out_path, c, PATHW - 1); out_path[PATHW - 1] = 0; } else join(out_path, PATHW, data, L"bbport.ini");
}
static const char *ini_value(const char *key) {
    static char value[512]; wchar_t path[PATHW]; config_path(path);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    char line[1024]; size_t k = strlen(key); const char *result = NULL;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!strncmp(line, key, k) && line[k] == '=') { snprintf(value, sizeof(value), "%s", line + k + 1); result = value; break; }
    }
    fclose(f);
    return result;
}
/* Replaces the key's line, or appends it; the file is rewritten whole and swapped in as the
 * in-game menu does. */
static void ini_set(const char *key, const char *value) {
    wchar_t path[PATHW], temp[PATHW]; config_path(path);
    _snwprintf(temp, PATHW, L"%ls.tmp", path); temp[PATHW - 1] = 0;
    FILE *in = _wfopen(path, L"rb"), *out = _wfopen(temp, L"wb");
    if (!out) { if (in) fclose(in); return; }
    int written = 0; char line[1024]; size_t k = strlen(key);
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            char copy[1024]; snprintf(copy, sizeof(copy), "%s", line); trim(copy);
            if (!strncmp(copy, key, k) && copy[k] == '=') { if (!written) fprintf(out, "%s=%s\n", key, value); written = 1; }
            else fputs(line, out);
        }
        fclose(in);
    } else fputs("# bbport settings (in-game menu: Insert / L3+R3)\n", out);
    if (!written) fprintf(out, "%s=%s\n", key, value);
    fclose(out);
    MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
static void patches_load(void) {
    const char *fps = ini_value("show_fps");
    settings.show_fps = !fps || !strcmp(fps, "1"); /* on until the menu or the launcher turns it off */
}

/* ---- the log: window, file, console ---- */
static HWND main_window, log_edit;
static FILE *log_file;
static HANDLE console_out;
#define WM_APP_LOG (WM_APP + 1)   /* lParam: a heap wchar_t* line for the log control */
#define WM_APP_DONE (WM_APP + 2)  /* wParam: the game's exit code */
static void log_write(const char *text, size_t n) {
    if (console_out && console_out != INVALID_HANDLE_VALUE) { DWORD written; WriteFile(console_out, text, (DWORD)n, &written, NULL); }
    if (log_file) { fwrite(text, 1, n, log_file); fflush(log_file); }
    if (main_window) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, text, (int)n, NULL, 0);
        wchar_t *w = malloc(((size_t)wn + 1) * sizeof(wchar_t));
        if (w) {
            MultiByteToWideChar(CP_UTF8, 0, text, (int)n, w, wn); w[wn] = 0;
            if (!PostMessageW(main_window, WM_APP_LOG, 0, (LPARAM)w)) free(w);
        }
    }
}
static void logf(const char *format, ...) {
    char text[4096]; va_list args; va_start(args, format);
    int n = vsnprintf(text, sizeof(text), format, args); va_end(args);
    if (n > 0) log_write(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
}
static void logw(const wchar_t *format, ...) {
    wchar_t text[4096]; va_list args; va_start(args, format);
    _vsnwprintf(text, 4096, format, args); va_end(args); text[4095] = 0;
    char utf8[16384]; int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n > 1) log_write(utf8, (size_t)n - 1);
}
/* Fatal before the game starts: the message in the log and a box (unless BB_NO_DIALOGS). */
static void fail(const wchar_t *message) {
    logw(L"%ls\n", message);
    if (!GetEnvironmentVariableW(L"BB_NO_DIALOGS", NULL, 0)) MessageBoxW(main_window, message, L"SacredFire", MB_OK | MB_ICONERROR);
}

/* ---- running programs ---- */
static HANDLE game_process; /* the running game (Stop) */
static volatile LONG stop_requested;
static int server_failed(void);
/* Quotes one argument the way CommandLineToArgvW undoes it. */
static void append_argument(wchar_t *line, size_t n, const wchar_t *argument) {
    size_t at = wcslen(line);
    if (at && at < n - 1) line[at++] = L' ';
    if (at < n - 1) line[at++] = L'"';
    size_t backslashes = 0;
    for (const wchar_t *p = argument; *p && at < n - 8; ++p) {
        if (*p == L'\\') { ++backslashes; line[at++] = *p; continue; }
        if (*p == L'"') { for (size_t i = 0; i <= backslashes && at < n - 8; ++i) line[at++] = L'\\'; }
        backslashes = 0;
        line[at++] = *p;
    }
    for (size_t i = 0; i < backslashes && at < n - 3; ++i) line[at++] = L'\\';
    line[at++] = L'"'; line[at] = 0;
}
/* Runs a command line and waits; its output goes to the log, or into `captured` (then at most
 * `wait_ms`). Returns the exit code, 127 when it cannot start. */
static DWORD run(wchar_t *line, char *captured, size_t capture_size, int is_game, DWORD wait_ms) {
    if (stop_requested) return 130;
    SECURITY_ATTRIBUTES inheritable = {.nLength = sizeof(inheritable), .bInheritHandle = TRUE};
    HANDLE read_end = NULL, write_end = NULL;
    if (!CreatePipe(&read_end, &write_end, &inheritable, 0)) { fail(L"CreatePipe failed"); return 127; }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup = {.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES, .hStdOutput = write_end, .hStdError = write_end};
    PROCESS_INFORMATION process;
    if (!captured) logw(L"> %ls\n", line);
    if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, root, &startup, &process)) {
        logw(L"cannot start %ls (error %lu)\n", line, GetLastError());
        CloseHandle(read_end); CloseHandle(write_end);
        return 127;
    }
    CloseHandle(process.hThread);
    CloseHandle(write_end);
    if (is_game) game_process = process.hProcess;
    size_t total = 0; DWORD got; char chunk[16384];
    for (;;) {
        if (stop_requested || (is_game && server_failed())) TerminateProcess(process.hProcess, 1);
        if (is_game) {
            DWORD available = 0;
            if (!PeekNamedPipe(read_end, NULL, 0, NULL, &available, NULL)) break;
            if (!available) {
                if (WaitForSingleObject(process.hProcess, 100) == WAIT_OBJECT_0) break;
                continue;
            }
        }
        if (captured) {
            if (total + 1 >= capture_size || !ReadFile(read_end, captured + total, (DWORD)(capture_size - 1 - total), &got, NULL) || !got) break;
            total += got;
        } else {
            if (!ReadFile(read_end, chunk, sizeof(chunk), &got, NULL) || !got) break;
            log_write(chunk, got);
        }
    }
    if (captured) captured[total] = 0;
    CloseHandle(read_end);
    if (WaitForSingleObject(process.hProcess, wait_ms) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    if (is_game) game_process = NULL;
    /* Windows could not load the program itself (its DLLs in bin\): say why in the log. */
    if (code == 0xC0000022u || code == 0xC0000135u || code == 0xC000007Bu || code == 0xC0000142u) {
        logw(L"%ls could not start: Windows error 0x%08lX (%ls).\n", line, code,
             code == 0xC0000022u ? L"access denied" : code == 0xC0000135u ? L"a DLL is missing" :
             code == 0xC000007Bu ? L"a DLL is not 64-bit" : L"a DLL failed to initialize");
        logw(L"  Usually: an antivirus blocked or quarantined a file in bin\\ (Windows Security > Protection\n"
             L"  history: allow it), the folder is synced by OneDrive (extract to e.g. C:\\Games\\UFC3Native),\n"
             L"  or the zip was extracted without being unblocked (zip > Properties > Unblock, extract again).\n");
    }
    CloseHandle(process.hProcess);
    return code;
}
/* python -E -s <script> args...: the bundled standard library and the script's folder only. */
static DWORD script(const wchar_t *folder, const wchar_t *name, const wchar_t *const *arguments, int count, char *captured, size_t capture_size) {
    static wchar_t line[32768];
    wchar_t path[PATHW];
    line[0] = 0;
    append_argument(line, 32768, python); append_argument(line, 32768, L"-E"); append_argument(line, 32768, L"-s");
    join(path, PATHW, root, folder); join(path, PATHW, path, name);
    append_argument(line, 32768, path);
    for (int i = 0; i < count; ++i) append_argument(line, 32768, arguments[i]);
    if (captured) logw(L"> %ls\n", line);
    return run(line, captured, capture_size, 0, INFINITE);
}

/* ---- Host server: tools\ufc3_server.py with the bundled Python, for one game session ----
 * Play in Host mode, after the preparation: the ports must be free (one in use is reported, never
 * taken over or closed: another UFC3.exe hosting, a ufc3_server.py); the server starts with
 * --bind 0.0.0.0, its output in <data>\logs\server-<time>.log; every listener has to answer on the
 * chosen address before the game starts, with BB_ONLINE_SERVER = that address, not loopback (the
 * runtime announces to the peer the interface that routes to the server: runtime_net.c
 * local_address). The server ends with the game (its exit, Stop, the window closed, a failed
 * start); it is in a job object that Windows kills with the launcher if the launcher is killed. */
static unsigned short host_tcp_ports[] = {42230, 40443, 40080, 10041, 10744, 17502}; /* ufc3_server.py --ports default */
static unsigned short host_udp_port = 17502;                                       /* ufc3_server.py QOS_PORT */
static DWORD host_start_timeout_ms = 30000; /* a first start can be slow (an antivirus scanning python\) */
static HANDLE server_job, server_process;    /* the play thread owns them; Stop and closing only kill */
static CRITICAL_SECTION server_lock;
static wchar_t server_log[PATHW];


/* ws2_32 and iphlpapi, loaded once in wWinMain (net_load). */
static int net_available;
static int (WSAAPI *p_WSAStartup)(WORD, LPWSADATA);
static int (WSAAPI *p_WSAGetLastError)(void);
static SOCKET (WSAAPI *p_socket)(int, int, int);
static int (WSAAPI *p_bind)(SOCKET, const struct sockaddr *, int);
static int (WSAAPI *p_connect)(SOCKET, const struct sockaddr *, int);
static int (WSAAPI *p_closesocket)(SOCKET);
static int (WSAAPI *p_ioctlsocket)(SOCKET, long, u_long *);
static int (WSAAPI *p_select)(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
static int (WSAAPI *p_setsockopt)(SOCKET, int, int, const char *, int);
static int (WSAAPI *p_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
static int (WSAAPI *p_recvfrom)(SOCKET, char *, int, int, struct sockaddr *, int *);
static ULONG (WINAPI *p_GetAdaptersAddresses)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
#define NET_LOAD(module, name) do { FARPROC f = GetProcAddress(module, #name); if (!f) return; memcpy(&p_##name, &f, sizeof f); } while (0)
static void net_load(void) {
    InitializeCriticalSection(&server_lock);
    HMODULE iphlp = LoadLibraryW(L"iphlpapi.dll");
    if (iphlp) { FARPROC f = GetProcAddress(iphlp, "GetAdaptersAddresses"); if (f) memcpy(&p_GetAdaptersAddresses, &f, sizeof f); }
    HMODULE ws2 = LoadLibraryW(L"ws2_32.dll");
    if (!ws2) return;
    NET_LOAD(ws2, WSAStartup); NET_LOAD(ws2, WSAGetLastError); NET_LOAD(ws2, socket); NET_LOAD(ws2, bind); NET_LOAD(ws2, connect);
    NET_LOAD(ws2, closesocket); NET_LOAD(ws2, ioctlsocket); NET_LOAD(ws2, select); NET_LOAD(ws2, setsockopt);
    NET_LOAD(ws2, sendto); NET_LOAD(ws2, recvfrom);
    WSADATA wsa;
    net_available = p_WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
/* IPv4 addresses are kept in host order (a.b.c.d = a << 24 | ...). */
static void make_sockaddr(struct sockaddr_in *a, uint32_t ip, unsigned short port) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = (unsigned short)((port >> 8) | (port << 8));
    unsigned char *b = (unsigned char *)&a->sin_addr;
    b[0] = (unsigned char)(ip >> 24); b[1] = (unsigned char)(ip >> 16); b[2] = (unsigned char)(ip >> 8); b[3] = (unsigned char)ip;
}
static void ip_text(uint32_t ip, wchar_t *out, size_t n) {
    _snwprintf(out, n, L"%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); out[n - 1] = 0;
}
/* Dotted IPv4 only (a, b, c, d in 0..255); 0 when it is not one. */
static int parse_ipv4(const wchar_t *text, uint32_t *ip) {
    uint32_t value = 0; int parts = 0;
    for (const wchar_t *p = text;; ++p) {
        if (!iswdigit(*p)) return 0;
        unsigned part = 0; int digits = 0;
        while (iswdigit(*p)) { part = part * 10 + (unsigned)(*p++ - L'0'); if (++digits > 3) return 0; }
        if (part > 255) return 0;
        value = value << 8 | part;
        if (++parts == 4) { if (*p) return 0; *ip = value; return 1; }
        if (*p != L'.') return 0;
    }
}
static int ip_private(uint32_t ip) { return (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8; }
static int ip_cgnat(uint32_t ip) { return (ip >> 22) == (100u << 2 | 1); } /* 100.64.0.0/10: Tailscale */
/* The first word of a field ("192.168.1.20  (LAN: Wi-Fi)" -> "192.168.1.20"). */
static void address_token(const wchar_t *text, wchar_t *out, size_t n) {
    while (*text == L' ' || *text == L'\t') ++text;
    size_t k = 0;
    while (text[k] && text[k] != L' ' && text[k] != L'\t' && text[k] != L'(' && k < n - 1) { out[k] = text[k]; ++k; }
    out[k] = 0;
}

/* This PC's IPv4 addresses (adapters that are up), best first: LAN, VPN, public, virtual. */
typedef struct { uint32_t ip; int rank; wchar_t text[16], label[192]; } LocalAddress;
static LocalAddress local_addresses[32];
static int local_address_count;
static const wchar_t *const rank_names[] = {L"LAN", L"VPN", L"public", L"virtual adapter"};
static int name_has(const wchar_t *name, const wchar_t *part) { return StrStrIW(name, part) != NULL; }
static void local_addresses_refresh(void) {
    local_address_count = 0;
    if (!p_GetAdaptersAddresses) return;
    ULONG size = 32768;
    IP_ADAPTER_ADDRESSES *list = NULL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        free(list); list = malloc(size);
        if (!list) return;
        const ULONG r = p_GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, list, &size);
        if (r == ERROR_SUCCESS) break;
        if (r != ERROR_BUFFER_OVERFLOW || attempt == 2) { free(list); return; }
    }
    for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        const wchar_t *name = a->FriendlyName ? a->FriendlyName : L"?";
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && local_address_count < COUNT(local_addresses); u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const unsigned char *b = (const unsigned char *)&((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr;
            const uint32_t ip = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
            if ((ip >> 24) == 127 || (ip >> 16) == 0xa9fe || !ip) continue; /* loopback, link-local (no DHCP) */
            int rank = ip_private(ip) ? 0 : ip_cgnat(ip) ? 1 : 2;
            if (name_has(name, L"Tailscale") || name_has(name, L"ZeroTier") || name_has(name, L"Hamachi") || name_has(name, L"Radmin") ||
                name_has(name, L"WireGuard") || name_has(name, L"NetBird")) rank = 1;
            else if (name_has(name, L"vEthernet") || name_has(name, L"VirtualBox") || name_has(name, L"VMware") || name_has(name, L"Hyper-V") ||
                     name_has(name, L"WSL") || name_has(name, L"Bluetooth")) rank = 3;
            int at = local_address_count++;
            while (at > 0 && local_addresses[at - 1].rank > rank) { local_addresses[at] = local_addresses[at - 1]; --at; }
            LocalAddress *l = &local_addresses[at];
            l->ip = ip; l->rank = rank;
            ip_text(ip, l->text, 16);
            _snwprintf(l->label, 192, L"%ls  (%ls: %ls)", l->text, rank_names[rank], name); l->label[191] = 0;
        }
    }
    free(list);
}
/* The Host address: an IPv4 address of this PC that the friend can reach. 0 and a message to
 * show when it is not usable; a public address is allowed (a note goes to the log). */
static int host_address_check(const wchar_t *text, uint32_t *ip, wchar_t *message, size_t n) {
    static const wchar_t pick[] = L"Pick this PC's LAN address (often 192.168.x.x) or, to play outside your home network, "
                                  L"its VPN address (Tailscale 100.x.x.x, ZeroTier) from the list; Rescan updates it.";
    if (!text[0]) { _snwprintf(message, n, L"Host server: no address for your friend. %ls", pick); message[n - 1] = 0; return 0; }
    if (!parse_ipv4(text, ip)) { _snwprintf(message, n, L"Host server: \"%ls\" is not an IPv4 address (like 192.168.1.20). %ls", text, pick); message[n - 1] = 0; return 0; }
    if ((*ip >> 24) == 127) {
        _snwprintf(message, n, L"Host server: %ls is this PC only (loopback): your friend cannot reach it. %ls", text, pick); message[n - 1] = 0; return 0;
    }
    if (!*ip || (*ip >> 28) >= 14 || (*ip >> 16) == 0xa9fe) {
        _snwprintf(message, n, L"Host server: %ls cannot be used by your friend (%ls). %ls", text,
                   (*ip >> 16) == 0xa9fe ? L"a link-local address: the network gave this PC no address" : L"not a PC's address", pick);
        message[n - 1] = 0; return 0;
    }
    local_addresses_refresh();
    int found = !p_GetAdaptersAddresses; /* no list on this system: the start checks it */
    for (int i = 0; i < local_address_count; ++i) found |= local_addresses[i].ip == *ip;
    if (!found) {
        _snwprintf(message, n, L"Host server: %ls is not an address of this PC now (is the network or the VPN connected?). %ls", text, pick);
        message[n - 1] = 0; return 0;
    }
    if (!ip_private(*ip) && !ip_cgnat(*ip))
        logw(L"Host server: %ls is not a LAN or Tailscale address; your friend reaches it only if this network routes it to this PC "
             L"(the launcher opens no router ports).\n", text);
    return 1;
}
/* A port someone else holds (bind fails: in use, or reserved by Windows / Hyper-V). */
static int port_taken(int type, unsigned short port) {
    SOCKET s = p_socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    struct sockaddr_in a; make_sockaddr(&a, 0, port);
    int taken = 0;
    if (p_bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) { const int e = p_WSAGetLastError(); taken = e == WSAEADDRINUSE || e == WSAEACCES; }
    p_closesocket(s);
    return taken;
}
/* A TCP connection to ip:port within 300 ms. */
static int tcp_answers(uint32_t ip, unsigned short port) {
    SOCKET s = p_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    u_long nonblocking = 1; p_ioctlsocket(s, FIONBIO, &nonblocking);
    struct sockaddr_in a; make_sockaddr(&a, ip, port);
    int ok = p_connect(s, (struct sockaddr *)&a, sizeof(a)) == 0;
    if (!ok && p_WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writable = {.fd_count = 1, .fd_array = {s}}, failed = {.fd_count = 1, .fd_array = {s}};
        const struct timeval wait = {0, 300000};
        ok = p_select(0, NULL, &writable, &failed, &wait) > 0 && writable.fd_count == 1 && failed.fd_count == 0;
    }
    p_closesocket(s);
    return ok;
}
/* The QoS ping site echoes a datagram back. */
static int udp_echoes(uint32_t ip, unsigned short port) {
    SOCKET s = p_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    const DWORD timeout = 300; p_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    struct sockaddr_in a; make_sockaddr(&a, ip, port);
    static const char probe[] = "UFC3.exe host check";
    char reply[64];
    const int ok = p_sendto(s, probe, sizeof(probe) - 1, 0, (struct sockaddr *)&a, sizeof(a)) > 0 && p_recvfrom(s, reply, sizeof(reply), 0, NULL, NULL) > 0;
    p_closesocket(s);
    return ok;
}
/* The end of the server's log, into the launcher's log. */
static void server_log_tail(void) {
    FILE *f = _wfopen(server_log, L"rb");
    if (!f) return;
    static char tail[4096]; fseek(f, 0, SEEK_END);
    const long size = ftell(f), from = size > (long)sizeof(tail) - 1 ? size - (long)sizeof(tail) + 1 : 0;
    fseek(f, from, SEEK_SET);
    const size_t n = fread(tail, 1, sizeof(tail) - 1, f); tail[n] = 0; fclose(f);
    logw(L"--- the host server's log (%ls) ---\n", server_log); logf("%s%s", tail, n && tail[n - 1] == '\n' ? "" : "\n"); logw(L"---\n");
}
/* Ends the server's process tree (any thread; the handles stay with the play thread). */
static void server_kill(void) {
    EnterCriticalSection(&server_lock);
    if (server_job) TerminateJobObject(server_job, 1); else if (server_process) TerminateProcess(server_process, 1);
    LeaveCriticalSection(&server_lock);
}
/* The play thread: ends it if it runs, says how it ended, closes the handles. */
static void server_stop(const wchar_t *why) {
    if (!server_process) return;
    DWORD code = 0;
    const int was_running = WaitForSingleObject(server_process, 0) == WAIT_TIMEOUT;
    server_kill();
    WaitForSingleObject(server_process, 5000);
    GetExitCodeProcess(server_process, &code);
    if (was_running) logw(L"Host server: stopped (%ls); its log: %ls\n", why, server_log);
    else { logw(L"Host server: it had already ended (exit code %lu) before %ls.\n", code, why); server_log_tail(); }
    EnterCriticalSection(&server_lock);
    CloseHandle(server_process); server_process = NULL;
    if (server_job) { CloseHandle(server_job); server_job = NULL; }
    LeaveCriticalSection(&server_lock);
}
static int server_failed(void) {
    if (!server_process || WaitForSingleObject(server_process, 0) != WAIT_OBJECT_0) return 0;
    server_log_tail();
    server_stop(L"an unexpected server exit");
    fail(L"The hosted server stopped unexpectedly. This online session will close; see the server log.");
    return 1;
}
/* Starts the server and waits until it answers on `ip`. 0: ready; 1: failed (said); 2: Stop. */
static int server_start(uint32_t ip) {
    wchar_t address[16], script_path[PATHW], message[1024];
    ip_text(ip, address, 16);
    join(script_path, PATHW, root, L"tools\\ufc3_server.py");
    if (!exists(script_path)) { fail(L"tools\\ufc3_server.py is missing next to the launcher: the package is incomplete."); return 1; }
    if (!net_available) { fail(L"Host server: Windows networking (ws2_32.dll) is not available."); return 1; }
    /* Ports already held: not ours to take over or close. */
    wchar_t taken[256] = L""; size_t at = 0;
    for (int i = 0; i < COUNT(host_tcp_ports); ++i)
        if (port_taken(SOCK_STREAM, host_tcp_ports[i])) at += (size_t)_snwprintf(taken + at, 256 - at, L"%ls%u/TCP", at ? L", " : L"", host_tcp_ports[i]);
    if (port_taken(SOCK_DGRAM, host_udp_port)) at += (size_t)_snwprintf(taken + at, 256 - at, L"%ls%u/UDP", at ? L", " : L"", host_udp_port);
    if (at) {
        _snwprintf(message, 1024, L"Host server: port %ls already in use on this PC, so the server cannot start. Another UFC 3 server "
                                  L"is probably running (another UFC3.exe hosting, or ufc3_server.py): close it, or pick Join server "
                                  L"and type its address. (A port reserved by Windows shows in: netsh int ipv4 show excludedportrange tcp)", taken);
        message[1023] = 0; fail(message); return 1;
    }
    /* <data>\logs\server-<time>.log: the server's output (also kept after a failure). */
    wchar_t logs[PATHW]; join(logs, PATHW, data, L"logs"); ensure_directory(logs);
    SYSTEMTIME now; GetLocalTime(&now);
    _snwprintf(server_log, PATHW, L"%ls\\server-%04u%02u%02u_%02u%02u%02u.log", logs, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    server_log[PATHW - 1] = 0;
    SECURITY_ATTRIBUTES inheritable = {.nLength = sizeof(inheritable), .bInheritHandle = TRUE};
    HANDLE log = CreateFileW(server_log, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) { _snwprintf(message, 1024, L"Host server: cannot write %ls", server_log); message[1023] = 0; fail(message); return 1; }
    /* python -E -s -u -X utf8 tools\ufc3_server.py --bind 0.0.0.0 --ports ...: one argument each, no shell. */
    static wchar_t line[32768]; wchar_t ports[128] = L"";
    for (int i = 0, k = 0; i < COUNT(host_tcp_ports); ++i) k += _snwprintf(ports + k, 128 - k, L"%ls%u", i ? L"," : L"", host_tcp_ports[i]);
    line[0] = 0;
    append_argument(line, 32768, python);
    const wchar_t *const options[] = {L"-E", L"-s", L"-u", L"-X", L"utf8"};
    for (int i = 0; i < COUNT(options); ++i) append_argument(line, 32768, options[i]);
    append_argument(line, 32768, script_path);
    append_argument(line, 32768, L"--bind"); append_argument(line, 32768, L"0.0.0.0");
    append_argument(line, 32768, L"--ports"); append_argument(line, 32768, ports);
    {
        char header[PATHW * 3 + 64]; char utf8[PATHW * 3];
        WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
        DWORD written; const int n = snprintf(header, sizeof(header), "UFC3.exe host server for %ls: %s\r\n", address, utf8);
        if (n > 0) WriteFile(log, header, (DWORD)strlen(header), &written, NULL);
    }
    logw(L"Host server: starting (log: %ls)\n> %ls\n", server_log, line);
    /* The job: the server dies with the launcher, even when the launcher is killed. */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { CloseHandle(job); job = NULL; }
    }
    /* The child inherits the log handle only (not the console, not other pipes). */
    STARTUPINFOEXW startup = {.StartupInfo = {.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES, .hStdOutput = log, .hStdError = log}};
    SIZE_T list_size = 0; InitializeProcThreadAttributeList(NULL, 1, 0, &list_size);
    LPPROC_THREAD_ATTRIBUTE_LIST list = list_size ? malloc(list_size) : NULL;
    if (list && (!InitializeProcThreadAttributeList(list, 1, 0, &list_size) ||
                 !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log, sizeof(log), NULL, NULL))) { free(list); list = NULL; }
    startup.lpAttributeList = list;
    if (!list) startup.StartupInfo.cb = sizeof(STARTUPINFOW);
    PROCESS_INFORMATION process;
    const BOOL started = CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | (list ? EXTENDED_STARTUPINFO_PRESENT : 0),
                                        NULL, data, &startup.StartupInfo, &process);
    const DWORD start_error = GetLastError();
    if (list) { DeleteProcThreadAttributeList(list); free(list); }
    CloseHandle(log); /* the child has its own: no later child (the game) inherits it */
    if (!started) {
        if (job) CloseHandle(job);
        _snwprintf(message, 1024, L"Host server: python\\python.exe could not start (Windows error %lu).", start_error); message[1023] = 0;
        fail(message); return 1;
    }
    if (job && !AssignProcessToJobObject(job, process.hProcess)) {
        logw(L"Host server: no job object (error %lu); it is ended with the game, but not if the launcher is killed.\n", GetLastError());
        CloseHandle(job); job = NULL;
    }
    EnterCriticalSection(&server_lock);
    server_job = job; server_process = process.hProcess;
    LeaveCriticalSection(&server_lock);
    ResumeThread(process.hThread); CloseHandle(process.hThread);
    /* Ready when every listener answers on the address the game and the friend use. */
    logw(L"Host server: waiting for its ports on %ls (TCP %ls, UDP %u; the checks show in its log)\n", address, ports, host_udp_port);
    int tcp_ready[COUNT(host_tcp_ports)] = {0}, udp_ready = 0;
    const ULONGLONG begin = GetTickCount64();
    for (;;) {
        if (stop_requested) { server_stop(L"Stop"); return 2; }
        if (WaitForSingleObject(server_process, 0) == WAIT_OBJECT_0) {
            DWORD code = 0; GetExitCodeProcess(server_process, &code);
            server_log_tail();
            server_stop(L"the game started");
            _snwprintf(message, 1024, L"Host server: it stopped while starting (exit code %lu). Its log is above and in %ls.", code, server_log);
            message[1023] = 0; fail(message); return 1;
        }
        int ready = 1;
        for (int i = 0; i < COUNT(host_tcp_ports); ++i) { if (!tcp_ready[i]) tcp_ready[i] = tcp_answers(ip, host_tcp_ports[i]); ready &= tcp_ready[i]; }
        if (!udp_ready) udp_ready = udp_echoes(ip, host_udp_port);
        if (ready && udp_ready) break;
        if (GetTickCount64() - begin > host_start_timeout_ms) {
            wchar_t missing[256] = L""; size_t m = 0;
            for (int i = 0; i < COUNT(host_tcp_ports); ++i)
                if (!tcp_ready[i]) m += (size_t)_snwprintf(missing + m, 256 - m, L"%ls%u/TCP", m ? L", " : L"", host_tcp_ports[i]);
            if (!udp_ready) m += (size_t)_snwprintf(missing + m, 256 - m, L"%ls%u/UDP", m ? L", " : L"", host_udp_port);
            server_log_tail();
            server_stop(L"it did not answer in time");
            _snwprintf(message, 1024, L"Host server: no answer on %ls port %ls after %lu s, so it was stopped. A firewall or security "
                                      L"program may block python.exe, or %ls is not this PC's address. Log: %ls",
                       address, missing, host_start_timeout_ms / 1000, address, server_log);
            message[1023] = 0; fail(message); return 1;
        }
        Sleep(200);
    }
    logw(L"Host server: ready on %ls (%.1f s).\n", address, (GetTickCount64() - begin) / 1000.0);
    logw(L"  Your friend: Online play = Join server, address %ls (outside your home network, both PCs on the same VPN).\n"
         L"  If Windows asks whether python.exe may accept connections, allow it (Private networks; a VPN can count as Public).\n", address);
    return 0;
}

/* ---- the controllers (bb-gpu-capabilities --gamepads: "GUID<tab>name" per line) ---- */
typedef struct { wchar_t guid[64], name[128]; } Gamepad;
static Gamepad gamepads[16]; static int gamepad_count;
static void gamepads_refresh(void) {
    gamepad_count = 0;
    if (!exists(caps_exe)) return;
    static wchar_t line[32768]; line[0] = 0;
    append_argument(line, 32768, caps_exe); append_argument(line, 32768, L"--gamepads");
    static char out[16384];
    DWORD code = run(line, out, sizeof(out), 0, 10000);
    /* Access denied right after extracting: often an antivirus still scanning the new files. */
    if (code == 0xC0000022u) { Sleep(2000); code = run(line, out, sizeof(out), 0, 10000); }
    if (code != 0) return;
    for (char *p = out; *p && gamepad_count < 16;) {
        char *end = strchr(p, '\n'); if (end) *end = 0;
        char *tab = strchr(p, '\t');
        if (tab) {
            *tab = 0;
            MultiByteToWideChar(CP_UTF8, 0, p, -1, gamepads[gamepad_count].guid, 64);
            MultiByteToWideChar(CP_UTF8, 0, tab + 1, -1, gamepads[gamepad_count].name, 128);
            gamepads[gamepad_count].guid[63] = gamepads[gamepad_count].name[127] = 0;
            trimw(gamepads[gamepad_count].name);
            ++gamepad_count;
        }
        if (!end) break;
        p = end + 1;
    }
}

/* ---- the run: what run.sh does, then the game. Returns the game's exit code. ---- */
static DWORD play_impl(const wchar_t *const *extra, int extra_count) {
    wchar_t out[PATHW], user_dir[PATHW], config[PATHW];
    join(out, PATHW, data, L"out"); ensure_directory(out);
    join(user_dir, PATHW, data, L"user"); ensure_directory(user_dir);
    config_path(config);
    if (!exists(python)) { fail(L"python\\python.exe is missing next to the launcher: the package is incomplete."); return 2; }
    if (!exists(game_exe)) { fail(L"bin\\sacredfire-game.exe is missing next to the launcher: the package is incomplete."); return 2; }
    SetEnvironmentVariableW(L"BB_CONFIG", config);
    SetEnvironmentVariableW(L"BB_USER_DIR", env_or(L"BB_USER_DIR", user_dir));
    SetEnvironmentVariableW(L"BB_GPU_USER_DIR", env_or(L"BB_GPU_USER_DIR", user_dir));
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    const wchar_t *game = settings.game;
    wchar_t eboot[PATHW]; join(eboot, PATHW, game, L"eboot.bin");
    if (!game[0] || !exists(eboot)) {
        wchar_t m[PATHW + 128];
        _snwprintf(m, PATHW + 128, L"No eboot.bin in \"%ls\": choose the game folder (CUSA06534, with CUSA06534-patch beside it or merged in).", game);
        m[PATHW + 127] = 0; fail(m); return 2;
    }
    /* Base and update side by side (CUSA06534 + CUSA06534-patch, as PKG extractors leave them):
     * merge_update.py links them into CUSA06534-merged once, and that folder is played. */
    static wchar_t merged_game[PATHW];
    {
        static char merged[PATHW * 3];
        const wchar_t *args[] = {game};
        if (script(L"scripts", L"merge_update.py", args, 1, merged, sizeof(merged))) {
            logf("%s", merged);
            fail(L"Merging the 1.14 update into the game folder failed (see the log).");
            return 2;
        }
        /* The folder is the last line (progress lines come before it). */
        trim(merged);
        char *last = strrchr(merged, '\n');
        if (last) { *last = 0; logf("%s\n", merged); last += 1; } else last = merged;
        MultiByteToWideChar(CP_UTF8, 0, last, -1, merged_game, PATHW); merged_game[PATHW - 1] = 0;
        trimw(merged_game);
        if (merged_game[0]) game = merged_game;
        logw(L"Game folder: %ls\n", game);
    }
    char profile[256] = "";
    if (wcscmp(env_or(L"BB_SKIP_GAME_CHECK", L"0"), L"1")) {
        const wchar_t *args[] = {game};
        if (script(L"scripts", L"game_check.py", args, 1, profile, sizeof(profile))) {
            logf("%s", profile);
            fail(L"The game check failed (see the log): the folder is not EA SPORTS UFC 3 CUSA06534 with the 1.14 update (CUSA06534-patch) beside it or merged in.");
            return 2;
        }
        trim(profile);
    }
    if (!profile[0]) snprintf(profile, sizeof(profile), "ufc3");
    /* The update's build (5.05, 6.72+ or ?): the address-keyed diagnostics know the 5.05 build only. */
    {
        char variant[64] = "?";
        const wchar_t *args[] = {game, L"--variant"};
        if (script(L"scripts", L"game_check.py", args, 2, variant, sizeof(variant)) == 0) trim(variant);
        if (!variant[0]) snprintf(variant, sizeof(variant), "?");
        wchar_t variant_w[64]; MultiByteToWideChar(CP_UTF8, 0, variant, -1, variant_w, 64); variant_w[63] = 0;
        SetEnvironmentVariableW(L"BB_GAME_VARIANT", variant_w);
    }
    wchar_t profile_w[64]; MultiByteToWideChar(CP_UTF8, 0, profile, -1, profile_w, 64); profile_w[63] = 0;
    SetEnvironmentVariableW(L"BB_GAME_PROFILE", profile_w);
    logw(L"Game profile: %ls\n", profile_w);
    /* Preparation (scripts/ as run.sh runs them; mod overlays need symlinks and are not applied here). */
    const wchar_t *prepare[] = {game, L"--out", out};
    if (script(L"scripts", L"prepare.py", prepare, 3, NULL, 0)) { fail(L"prepare.py failed (see the log)"); return 2; }
    if (script(L"scripts", L"link_libc.py", prepare, 3, NULL, 0)) { fail(L"link_libc.py failed (see the log)"); return 2; }
    const wchar_t *link_args[] = {game, L"--out", out, L"--modules", L"libc.prx", L"libSceFios2.prx"};
    if (script(L"scripts", L"link_modules.py", link_args, 6, NULL, 0)) { fail(L"link_modules.py failed (see the log)"); return 2; }
    wchar_t aerolib[PATHW]; join(aerolib, PATHW, root, L"share\\aerolib.inl");
    const wchar_t *names_args[] = {L"--out", out, L"--aerolib", aerolib};
    if (script(L"tools", L"gen_import_names.py", names_args, 4, NULL, 0)) { fail(L"gen_import_names.py failed (see the log)"); return 2; }
    const wchar_t *content_args[] = {game, L"--out", out, L"--sku", env_or(L"BB_CONTENT_SKU", L"full")};
    if (script(L"scripts", L"content_profile.py", content_args, 5, NULL, 0)) { fail(L"content_profile.py failed (see the log)"); return 2; }
    /* 2K / 4K: the experiment's folder and json instead of the community patches (as
     * tools/ufc45-4k-test.sh and ufc46-1440p-test.sh run them); a BB_PATCHES_DIR or
     * BB_PATCHES_CONFIG the launcher was started with wins. */
    const wchar_t *experiment = resolution_experiment[settings.resolution];
    wchar_t patches_dir[PATHW], patches_config[PATHW];
    if (experiment) {
        join(patches_dir, PATHW, root, L"patches\\experiments"); join(patches_dir, PATHW, patches_dir, experiment);
        join(patches_config, PATHW, patches_dir, resolution_experiment_config[settings.resolution]);
        if (!given_env(L"BB_PATCHES_CONFIG") && !exists(patches_config)) {
            wchar_t m[PATHW + 128]; _snwprintf(m, PATHW + 128, L"%ls is missing: the package is incomplete (2K / 4K need it).", patches_config);
            m[PATHW + 127] = 0; fail(m); return 2;
        }
    } else { join(patches_dir, PATHW, root, L"patches"); join(patches_config, PATHW, data, L"patches.json"); }
    const wchar_t *patches_dir_used = given_env(L"BB_PATCHES_DIR") ? given_env(L"BB_PATCHES_DIR") : patches_dir;
    const wchar_t *patches_config_used = given_env(L"BB_PATCHES_CONFIG") ? given_env(L"BB_PATCHES_CONFIG") : patches_config;
    const wchar_t *patch_args[] = {L"--patches-dir", patches_dir_used, L"--patches-config", patches_config_used,
                                   L"--app-version", L"01.14", L"--title-id", L"CUSA06534", L"--out", out, L"--settings", config};
    if (script(L"scripts", L"patches.py", patch_args, 12, NULL, 0)) { fail(L"patches.py failed (see the log)"); return 2; }
    /* The count of guest TCB loads the linker rewrote (link.json fs_loads_patched): the game
     * executable cross-checks its own scan against it. */
    wchar_t tls_loads[32] = L"-1";
    {
        wchar_t link_json[PATHW]; join(link_json, PATHW, out, L"link.json");
        FILE *f = _wfopen(link_json, L"rb");
        if (f) {
            static char text[1 << 20]; size_t n = fread(text, 1, sizeof(text) - 1, f); text[n] = 0; fclose(f);
            const char *key = strstr(text, "\"fs_loads_patched\"");
            if (key && (key = strchr(key, ':'))) _snwprintf(tls_loads, 32, L"%ld", strtol(key + 1, NULL, 10));
        }
    }
    /* The switches (the window's, or the saved ones with --play) and run.sh's defaults for the
     * ufc3 profile. An environment variable set before the launcher wins over each of them. */
    game_env(L"BB_FULLSCREEN", settings.fullscreen ? L"1" : NULL);
    game_env(L"BB_VK_VALIDATION", settings.validation ? L"1" : NULL);
    game_env(L"BB_FRAME_STATS", settings.frame_stats ? L"1" : NULL);
    if (settings.frame_stats) {
        /* As run.sh: with the frame statistics on, the per-frame and readback CSVs of the
         * session go beside the run's log (the effective log folder). */
        wchar_t logs[PATHW], csv[PATHW]; effective_log_dir(logs); ensure_directory(logs);
        SYSTEMTIME now; GetLocalTime(&now);
        _snwprintf(csv, PATHW, L"%ls\\%04u%02u%02u_%02u%02u%02u.frames.csv", logs, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        csv[PATHW - 1] = 0; game_env(L"BB_FRAME_LOG", csv);
        _snwprintf(csv, PATHW, L"%ls\\%04u%02u%02u_%02u%02u%02u.readbacks.csv", logs, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        csv[PATHW - 1] = 0; game_env(L"BB_READBACK_LOG", csv);
    }
    game_env(L"BB_MENU_TRACE", settings.menu_trace ? L"1" : NULL);
    game_env(L"BB_GAMEPAD", settings.gamepad[0] ? settings.gamepad : NULL);
    game_env(L"BB_USER_NAME", settings.player[0] ? settings.player : NULL); /* the PS4 user name the game shows */
    game_env(L"BB_LANGUAGE", language_ids[settings.language]); /* a BB_LANGUAGE set before the launcher (a .bat) wins */
    const wchar_t *online = settings.online_mode == ONLINE_HOST ? settings.host_address :
                            settings.online_mode == ONLINE_JOIN ? settings.join_address : NULL;
    if (settings.online_mode == ONLINE_JOIN && !settings.join_address[0] && !given_env(L"BB_ONLINE_SERVER")) {
        fail(L"Join server: enter the host's IP address."); return 2;
    }
    if (settings.online_mode == ONLINE_HOST && given_env(L"BB_ONLINE_SERVER") && wcscmp(given_env(L"BB_ONLINE_SERVER"), settings.host_address)) {
        fail(L"BB_ONLINE_SERVER differs from the selected Host address. Remove the environment override or select that same local IP."); return 2;
    }
    game_env(L"BB_ONLINE_SERVER", online);
    /* The fight scene renders black without precise readbacks of linear images (shadPS4 issue
     * #4375, UFC 4, same engine); precise readbacks deadlock against the GPU write twins, so
     * those stay off. The game asks for flip rate 1 (30 Hz); its simulation follows the real
     * frame time, so flip rate 0 with a 480 Hz virtual vblank and the present thread's cap set
     * the rate. Z16+S8 shadow data uploaded from memory is kept as D16 (stencil dropped). */
    /* PS4 Pro mode: the game renders its scene at 1080p instead of 900p (CMask reads are
     * answered as uncompressed by the runtime). */
    game_env(L"BB_NEO", settings.neo || experiment ? L"1" : L"0"); /* 2K / 4K: always Pro (its 4K VideoOut) */
    game_env(L"BB_READBACKS", L"2"); game_env(L"BB_READBACK_LINEAR", L"1"); game_env(L"BB_GPU_WRITE_TWINS", L"0");
    /* Write windows and single-stepped writes on, as on Linux (run.sh): UFC-48 made the windows
     * coherent (the heap corruption of 0.1.4 is gone). Validated on Windows 2026-10-16 (RTX 3060):
     * Career fight ~47 FPS, blood and sweat, created fighter's skin, second fight and contract
     * without a crash. Their audit (BB_WRITE_WINDOW_DIAG) stays in the test launcher only. */
    game_env(L"BB_WRITE_WINDOW", L"1"); game_env(L"BB_WRITE_STEP", L"1");
    game_env(L"BB_FLIP_RATE", L"0"); game_env(L"BB_VBLANK_HZ", L"480");
    game_env(L"BB_Z16S8_AS_D16", L"1");
    /* Pre-upload restores blood and sweat on hits (UFC-49); validated on Windows together with
     * the write windows above. The user's environment wins over all three. */
    game_env(L"BB_PREUPLOAD", L"1");
#ifdef UFC3_WRITE_WINDOWS_TEST
    /* UFC-48 test launcher (UFC3-WRITE-WINDOWS-TEST.exe; UFC3.exe is the release one, unchanged):
     * write windows and single steps on, with their audit, to measure the coherent windows. */
    SetEnvironmentVariableW(L"BB_WRITE_STEP", L"1");
    SetEnvironmentVariableW(L"BB_WRITE_WINDOW", L"1");
    SetEnvironmentVariableW(L"BB_WRITE_WINDOW_DIAG", L"1");
    SetEnvironmentVariableW(L"BB_WRITE_WINDOW_DIAG_MAX", L"1000");
#endif
    game_env(L"BB_COPY_GPU_BUFFERS", L"1");
#ifdef UFC3_DIAGNOSTIC_PACKAGE
    /* The diagnostic variant (UFC-42) runs the write windows with their audit, forced, as the
     * .cmd of the diagnostic package did. */
    SetEnvironmentVariableW(L"BB_WRITE_STEP", L"1");
    SetEnvironmentVariableW(L"BB_WRITE_WINDOW", L"1");
    SetEnvironmentVariableW(L"BB_WRITE_WINDOW_DIAG", L"1");
    SetEnvironmentVariableW(L"BB_WRITE_LOG", L"0");
#endif
    game_env(L"BB_FPS_LIMIT", fps_limits[settings.fps_mode]);
    game_env(L"BB_PRESENT_MODE", present_modes[settings.present_mode]);
    game_env(L"BB_INTERNAL_WIDTH", resolution_w[settings.resolution]);
    game_env(L"BB_INTERNAL_HEIGHT", resolution_h[settings.resolution]);
    game_env(L"BB_WINDOW_SIZE", display_sizes[settings.display]);
    {
        wchar_t clip[PATHW]; join(clip, PATHW, root, L"share\\skip_movie.mkv");
        game_env(L"BB_SKIP_MOVIES", settings.skip_movies && exists(clip) ? L"1" : L"0");
        game_env(L"BB_SKIP_MOVIE_FILE", clip);
    }
    game_env(L"BB_FSR1", settings.upscaler == 1 ? L"1" : L"0");
    game_env(L"BB_RCAS", settings.sharpness ? L"1" : L"0");
    game_env(L"BB_RCAS_ATTENUATION", sharpness_attenuation[settings.sharpness]);
    SetEnvironmentVariableW(L"BB_UFFD", L"0");            /* Linux only */
    SetEnvironmentVariableW(L"BB_GUEST_IN_PLACE", L"0");  /* dma-buf memory model: Linux only */
    /* env_or keeps 8 values at a time: two lines. */
    logw(L"UFC 3: output %lsx%ls, frame limit %ls (0: none), present mode %ls, fullscreen %ls\n",
         env_or(L"BB_INTERNAL_WIDTH", L"?"), env_or(L"BB_INTERNAL_HEIGHT", L"?"), env_or(L"BB_FPS_LIMIT", L"?"),
         env_or(L"BB_PRESENT_MODE", L"?"), env_or(L"BB_FULLSCREEN", L"0"));
    logw(L"UFC 3: player name \"%ls\", language %ls (BB_LANGUAGE)\n", env_or(L"BB_USER_NAME", L"Player 1"), env_or(L"BB_LANGUAGE", L"1"));
    {
        const wchar_t *server = env_or(L"BB_ONLINE_SERVER", L"");
        if (server[0]) logw(L"UFC 3: online server %ls (BB_ONLINE_SERVER)\n", server); else logw(L"UFC 3: offline (no BB_ONLINE_SERVER)\n");
    }
    if (experiment)
        logw(L"4K/2K experimental: in game choose VIDEO -> Favor Resolution (the scene grows only with it). %ls scene, PS4 Pro %ls, "
             L"patches %ls\n", resolution_names[settings.resolution], env_or(L"BB_NEO", L"?"), patches_dir_used);
    logw(L"UFC 3: window %ls, FSR 1.0 %ls, RCAS %ls (attenuation %ls), readbacks %ls (2 = precise)\n",
         env_or(L"BB_WINDOW_SIZE", L"= output"), env_or(L"BB_FSR1", L"0"), env_or(L"BB_RCAS", L"?"), env_or(L"BB_RCAS_ATTENUATION", L"?"), env_or(L"BB_READBACKS", L"?"));
    if (settings.clear_cache || !wcscmp(env_or(L"BB_CLEAR_SHADER_CACHE", L"0"), L"1")) {
        wchar_t cache[PATHW]; join(cache, PATHW, env_or(L"BB_USER_DIR", user_dir), L"cache\\CUSA06534");
        if (exists(cache)) {
            wchar_t command[PATHW + 64]; _snwprintf(command, PATHW + 64, L"cmd.exe /c rd /s /q \"%ls\"", cache); command[PATHW + 63] = 0;
            run(command, NULL, 0, 0, INFINITE);
        }
        logw(L"Shader cache: cleared for this launch\n");
    }
    /* The restart command: this program again (--play), after this game process is gone. */
    {
        static wchar_t restart[32768]; wchar_t self[PATHW], pid[32];
        GetModuleFileNameW(NULL, self, PATHW);
        restart[0] = 0;
        append_argument(restart, 32768, self); append_argument(restart, 32768, L"--after");
        _snwprintf(pid, 32, L"%lu", GetCurrentProcessId()); append_argument(restart, 32768, pid);
        append_argument(restart, 32768, L"--play");
        for (int i = 0; i < extra_count; ++i) append_argument(restart, 32768, extra[i]);
        SetEnvironmentVariableW(L"BB_RESTART_COMMAND", restart);
    }
    static wchar_t line[32768]; wchar_t path[PATHW];
    line[0] = 0;
    append_argument(line, 32768, game_exe);
    join(path, PATHW, out, L"boot-linked.bin"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--import-names"); join(path, PATHW, out, L"import_names.txt"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--content-profile"); join(path, PATHW, out, L"content.bin"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--patches"); join(path, PATHW, out, L"patches.bin"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--app0"); append_argument(line, 32768, game);
    append_argument(line, 32768, L"--user"); append_argument(line, 32768, env_or(L"BB_USER_DIR", user_dir));
    append_argument(line, 32768, L"--timeout"); append_argument(line, 32768, env_or(L"BB_TIMEOUT", L"0"));
    append_argument(line, 32768, L"--tls-loads"); append_argument(line, 32768, tls_loads);
    for (int i = 0; i < extra_count; ++i) append_argument(line, 32768, extra[i]);
    char version[64]; read_version(version, sizeof(version));
    logf("SacredFire %s (Windows)\n", version);
#ifdef UFC3_WRITE_WINDOWS_TEST
    logw(L"UFC-48 write windows test: BB_WRITE_STEP=1 BB_WRITE_WINDOW=1 BB_WRITE_WINDOW_DIAG=1 BB_WRITE_WINDOW_DIAG_MAX=1000 (UFC3-WRITE-WINDOWS-TEST.exe).\n");
#endif
#ifdef UFC3_DIAGNOSTIC_PACKAGE
    logw(L"Windows diagnostic test: write windows and their audit are on (BB_WRITE_STEP=1, BB_WRITE_WINDOW=1, BB_WRITE_WINDOW_DIAG=1, BB_WRITE_LOG=0).\n");
#endif
    if (stop_requested) return 130;
    if (settings.online_mode == ONLINE_HOST) {
        uint32_t ip; wchar_t message[1024];
        if (!host_address_check(settings.host_address, &ip, message, COUNT(message))) { fail(message); return 2; }
        if (server_start(ip)) return 2;
    }
    const DWORD code = run(line, NULL, 0, 1, INFINITE);
    logf("sacredfire-game.exe exited with code %lu%s\n", code,
         code == 0 ? "" : code == 3 ? " (a fault; the report is above)" : code == 142 ? " (watchdog)" : code == 23 ? " (GPU library assertion)" : "");
    return code;
}

static DWORD play(const wchar_t *const *extra, int extra_count) {
    const DWORD code = play_impl(extra, extra_count);
    server_stop(L"the game session ended");
    return code;
}

/* ---- Steam: a non-Steam shortcut (userdata\<user>\config\shortcuts.vdf) ----
 * The same entry the Linux launcher writes (launcher/steam_shortcuts.py): Steam keeps its
 * non-Steam games in a binary VDF (a map of numbered entries; strings 0x01, 32-bit integers
 * 0x02, nested maps 0x00 ended by 0x08) that it rewrites when it exits, so Steam has to be
 * closed while the entry is added. The app id is the CRC32 of the quoted executable and the
 * name with the top bit set, as Steam computes it. The shortcut starts SacredFire.exe --play. */
static uint32_t crc32_of(const char *text) {
    uint32_t crc = 0xffffffffu;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        crc ^= *p;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static int steam_running(void) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry = {.dwSize = sizeof(entry)};
    int found = 0;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
        if (!_wcsicmp(entry.szExeFile, L"steam.exe")) found = 1;
    CloseHandle(snapshot);
    return found;
}
static int steam_root(wchar_t *out_path) {
    DWORD size = PATHW * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, out_path, &size) != ERROR_SUCCESS) return 0;
    for (wchar_t *p = out_path; *p; ++p) if (*p == L'/') *p = L'\\';
    return exists(out_path);
}
/* A map's contents from `at` (just after its key) to its closing 0x08; -1 on a malformed file. */
static long vdf_skip_map(const unsigned char *d, long n, long at) {
    while (at < n) {
        unsigned char kind = d[at++];
        if (kind == 0x08) return at;
        while (at < n && d[at]) ++at; /* the key */
        if (at++ >= n) return -1;
        if (kind == 0x00) { at = vdf_skip_map(d, n, at); if (at < 0) return -1; }
        else if (kind == 0x01) { while (at < n && d[at]) ++at; if (at++ >= n) return -1; }
        else if (kind == 0x02) at += 4;
        else return -1;
    }
    return -1;
}
static void vdf_put(unsigned char **out, const void *bytes, size_t n) { memcpy(*out, bytes, n); *out += n; }
static void vdf_string(unsigned char **out, const char *key, const char *value) { unsigned char k = 1; vdf_put(out, &k, 1); vdf_put(out, key, strlen(key) + 1); vdf_put(out, value, strlen(value) + 1); }
static void vdf_int(unsigned char **out, const char *key, int32_t value) { unsigned char k = 2; vdf_put(out, &k, 1); vdf_put(out, key, strlen(key) + 1); vdf_put(out, &value, 4); }
/* Adds the entry to one shortcuts.vdf (created if missing; an entry with the same executable
 * and name is replaced). Returns 1 added, 2 replaced, 0 on failure (message in the log). */
static int steam_add_shortcut(const wchar_t *path, const char *exe_quoted, const char *start_dir_quoted) {
    static unsigned char data[1 << 20], fresh[4096];
    long n = 0;
    FILE *f = _wfopen(path, L"rb");
    if (f) { n = (long)fread(data, 1, sizeof(data), f); fclose(f); if (n >= (long)sizeof(data)) { logw(L"%ls: too large\n", path); return 0; } }
    /* The new entry. */
    char appid_text[1200]; snprintf(appid_text, sizeof(appid_text), "%sSacredFire", exe_quoted);
    const int32_t appid = (int32_t)(crc32_of(appid_text) | 0x80000000u);
    unsigned char entry[4096], *e = entry;
    unsigned char kind_map = 0, end = 8;
    char index_key[16] = "0";
    /* Walk the existing file: the "shortcuts" map's entries. */
    long body_start = -1, body_end = -1, replace_start = -1, replace_end = -1; int next_index = 0;
    if (n > 0) {
        long at = 0;
        if (data[at] != 0x00 || strcmp((const char *)data + 1, "shortcuts") != 0) { logw(L"%ls: not a shortcuts file\n", path); return 0; }
        at = 1 + (long)strlen("shortcuts") + 1;
        body_start = at;
        while (at < n && data[at] != 0x08) {
            if (data[at] != 0x00) { logw(L"%ls: unexpected field in the shortcuts map\n", path); return 0; }
            const long entry_start = at;
            const char *key = (const char *)data + at + 1;
            long inner = at + 1 + (long)strlen(key) + 1;
            const long entry_end = vdf_skip_map(data, n, inner);
            if (entry_end < 0) { logw(L"%ls: malformed\n", path); return 0; }
            const int index = atoi(key); if (index + 1 > next_index) next_index = index + 1;
            /* Its Exe and AppName. */
            const char *exe = NULL, *name = NULL;
            for (long k = inner; k < entry_end - 1;) {
                unsigned char kind = data[k++]; const char *field = (const char *)data + k; k += (long)strlen(field) + 1;
                if (kind == 0x01) { const char *value = (const char *)data + k; k += (long)strlen(value) + 1; if (!strcmp(field, "Exe")) exe = value; else if (!strcmp(field, "AppName")) name = value; }
                else if (kind == 0x02) k += 4;
                else if (kind == 0x00) { k = vdf_skip_map(data, n, k); if (k < 0) break; }
                else break;
            }
            if (exe && name && !strcmp(exe, exe_quoted) && !strcmp(name, "SacredFire") && replace_start < 0) { replace_start = entry_start; replace_end = entry_end; snprintf(index_key, sizeof(index_key), "%s", key); }
            at = entry_end;
        }
        if (at >= n) { logw(L"%ls: malformed (no end)\n", path); return 0; }
        body_end = at;
    }
    if (replace_start < 0) snprintf(index_key, sizeof(index_key), "%d", next_index);
    vdf_put(&e, &kind_map, 1); vdf_put(&e, index_key, strlen(index_key) + 1);
    vdf_int(&e, "appid", appid);
    vdf_string(&e, "AppName", "SacredFire");
    vdf_string(&e, "Exe", exe_quoted);
    vdf_string(&e, "StartDir", start_dir_quoted);
    vdf_string(&e, "icon", "");
    vdf_string(&e, "ShortcutPath", "");
    vdf_string(&e, "LaunchOptions", "--play");
    vdf_int(&e, "IsHidden", 0); vdf_int(&e, "AllowDesktopConfig", 1); vdf_int(&e, "AllowOverlay", 1);
    vdf_int(&e, "OpenVR", 0); vdf_int(&e, "Devkit", 0);
    vdf_string(&e, "DevkitGameID", "");
    vdf_int(&e, "DevkitOverrideAppID", 0); vdf_int(&e, "LastPlayTime", 0);
    vdf_string(&e, "FlatpakAppID", "");
    vdf_put(&e, &kind_map, 1); vdf_put(&e, "tags", 5); vdf_put(&e, &end, 1);
    vdf_put(&e, &end, 1);
    const size_t entry_bytes = (size_t)(e - entry);
    /* The new file. */
    wchar_t temp[PATHW]; _snwprintf(temp, PATHW, L"%ls.tmp", path); temp[PATHW - 1] = 0;
    wchar_t folder[PATHW]; wcsncpy(folder, path, PATHW - 1); folder[PATHW - 1] = 0; { wchar_t *slash = wcsrchr(folder, L'\\'); if (slash) *slash = 0; } ensure_directory(folder);
    FILE *out = _wfopen(temp, L"wb");
    if (!out) { logw(L"%ls: cannot write\n", temp); return 0; }
    int result;
    if (n == 0) {
        unsigned char *h = fresh;
        vdf_put(&h, &kind_map, 1); vdf_put(&h, "shortcuts", 10);
        fwrite(fresh, 1, (size_t)(h - fresh), out); fwrite(entry, 1, entry_bytes, out); fputc(8, out); fputc(8, out);
        result = 1;
    } else if (replace_start >= 0) {
        fwrite(data, 1, (size_t)replace_start, out); fwrite(entry, 1, entry_bytes, out); fwrite(data + replace_end, 1, (size_t)(n - replace_end), out);
        result = 2;
    } else {
        fwrite(data, 1, (size_t)body_end, out); fwrite(entry, 1, entry_bytes, out); fwrite(data + body_end, 1, (size_t)(n - body_end), out);
        result = 1;
    }
    (void)body_start;
    fclose(out);
    if (!MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { logw(L"%ls: cannot replace\n", path); return 0; }
    return result;
}
/* The button: every Steam user of this Steam installation gets the shortcut. */
static void add_to_steam(void) {
    if (steam_running()) { fail(L"Close Steam first: it overwrites its shortcuts when it exits. Then press Add to Steam again."); return; }
    wchar_t steam[PATHW], userdata[PATHW], pattern[PATHW];
    if (!steam_root(steam)) { fail(L"Steam was not found (no SteamPath in the registry)."); return; }
    join(userdata, PATHW, steam, L"userdata"); join(pattern, PATHW, userdata, L"*");
    wchar_t self[PATHW]; GetModuleFileNameW(NULL, self, PATHW);
    char exe_utf8[PATHW * 3], root_utf8[PATHW * 3], exe_quoted[PATHW * 3 + 4], root_quoted[PATHW * 3 + 4];
    WideCharToMultiByte(CP_UTF8, 0, self, -1, exe_utf8, sizeof(exe_utf8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, root, -1, root_utf8, sizeof(root_utf8), NULL, NULL);
    snprintf(exe_quoted, sizeof(exe_quoted), "\"%s\"", exe_utf8); snprintf(root_quoted, sizeof(root_quoted), "\"%s\"", root_utf8);
    int added = 0, replaced = 0, failed = 0;
    WIN32_FIND_DATAW found; HANDLE search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !iswdigit(found.cFileName[0]) || !wcscmp(found.cFileName, L"0")) continue;
            wchar_t config[PATHW], vdf[PATHW];
            join(config, PATHW, userdata, found.cFileName); join(config, PATHW, config, L"config");
            if (!exists(config)) continue;
            join(vdf, PATHW, config, L"shortcuts.vdf");
            const int r = steam_add_shortcut(vdf, exe_quoted, root_quoted);
            if (r == 1) ++added; else if (r == 2) ++replaced; else ++failed;
            logw(L"Steam: %ls: %ls\n", vdf, r == 1 ? L"shortcut added" : r == 2 ? L"shortcut updated" : L"failed");
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    if (added + replaced == 0) { fail(failed ? L"The Steam shortcut could not be written (see the log)." : L"No Steam user folder was found (Steam\\userdata)."); return; }
    wchar_t message[256];
    _snwprintf(message, 256, L"Added to Steam as \"SacredFire\" (%d added, %d updated). Start Steam to see it; it starts the game with the saved settings (--play).", added, replaced);
    logw(L"%ls\n", message);
    if (!GetEnvironmentVariableW(L"BB_NO_DIALOGS", NULL, 0)) MessageBoxW(main_window, message, L"SacredFire", MB_OK | MB_ICONINFORMATION);
}

/* ---- the window ---- */
enum { ID_GAME = 100, ID_BROWSE, ID_PLAYER, ID_LANGUAGE, ID_ONLINE, ID_MODE, ID_HOST, ID_HOSTSCAN, ID_COPYHOST, ID_GAMEPAD, ID_RESCAN, ID_CONTROLS, ID_RESOLUTION, ID_DISPLAY, ID_FPS, ID_PRESENT, ID_UPSCALER, ID_SHARPNESS, ID_SHOWFPS, ID_FULLSCREEN, ID_NEO, ID_SKIPMOVIES, ID_CLEAR, ID_VALIDATION, ID_STATS, ID_TRACE, ID_LOGDIR, ID_LOGDIR_BROWSE,
       ID_PLAY, ID_STOP, ID_STEAM, ID_LOGS, ID_DATA, ID_LOG, ID_LAST };
static HWND controls[ID_LAST - 100];
static int running;
static float scale = 1.0f;
static HFONT ui_font; /* Segoe UI at the system DPI (the Controls window uses it too) */
static int sx(int v) { return (int)(v * scale + 0.5f); }
static HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND h_wnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, sx(x), sx(y), sx(w), sx(h), main_window, (HMENU)(INT_PTR)id,
                                 GetModuleHandleW(NULL), NULL);
    SendMessageW(h_wnd, WM_SETFONT, (WPARAM)ui_font, TRUE);
    if (id >= 100 && id < ID_LAST) controls[id - 100] = h_wnd;
    return h_wnd;
}
static HWND control(int id) { return controls[id - 100]; }

/* ---- the look: the UFC 3 retail cover (near-black stadium, gold belts, UFC red) ----
 * The window paints the background, the banner and the sections; statics, edits and lists take
 * their colours from WM_CTLCOLOR*; buttons and check boxes are owner-drawn (the themed ones
 * ignore text colours). Red is for Play and Stop only. */
#define HEX(v) RGB(((v) >> 16) & 255, ((v) >> 8) & 255, (v) & 255)
#define C_BACKGROUND HEX(0x0B0B0D)
#define C_PANEL HEX(0x16161A)
#define C_BORDER HEX(0x2A2A30)
#define C_INPUT HEX(0x1F1F25)
#define C_LOG HEX(0x101013)
#define C_GOLD HEX(0xC9A24A)
#define C_GOLD_LIGHT HEX(0xE8C872)
#define C_TEXT HEX(0xF2F2F2)
#define C_TEXT_DIM HEX(0xA0A0A8)
#define C_TEXT_OFF HEX(0x5C5C66)
#define C_RED HEX(0xD20A0A)
enum { CLIENT_W = 900, CLIENT_H = 884, BANNER_H = 120 };
/* The sections, at 96 DPI (sx scales them). */
static const struct { int x, y, w, h; const wchar_t *title; } panels[] = {
    {16, 134, 426, 214, L"GAME"}, {16, 360, 426, 220, L"ONLINE"},
    {458, 134, 426, 330, L"VIDEO"}, {458, 476, 426, 104, L"ADVANCED"},
    {16, 592, 868, 88, L"LOGS"}};
static HBRUSH brush_background, brush_panel, brush_input, brush_log;
static HFONT font_section, font_title, font_small, font_log, font_action;
static int hot_id;                       /* the owner-drawn button under the mouse */
static unsigned char check_state[ID_LAST - 100], is_check[ID_LAST - 100];
static HWND notes[8]; static int note_count; /* secondary text */
static HBITMAP banner_art; static int banner_art_w, banner_art_h;
static int checked(int id) { return check_state[id - 100]; }
static void set_checked(int id, int on) {
    if (check_state[id - 100] == !!on) return;
    check_state[id - 100] = (unsigned char)!!on;
    InvalidateRect(control(id), NULL, FALSE);
}
static HFONT make_font(int height, int weight, const wchar_t *face) {
    LOGFONTW f = {.lfHeight = -sx(height), .lfWeight = weight, .lfCharSet = DEFAULT_CHARSET, .lfQuality = CLEARTYPE_QUALITY};
    wcsncpy(f.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&f);
}
/* uxtheme / dwmapi, loaded when present: dark scroll bars and lists, a dark title bar. */
static HRESULT (WINAPI *p_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
static void dark_theme(HWND h, const wchar_t *name) { if (p_SetWindowTheme) p_SetWindowTheme(h, name, NULL); }
static void theme_load(HWND window) {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (ux) { FARPROC f = GetProcAddress(ux, "SetWindowTheme"); if (f) memcpy(&p_SetWindowTheme, &f, sizeof f); }
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    FARPROC f = dwm ? GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
    if (!f) return;
    HRESULT (WINAPI *set)(HWND, DWORD, LPCVOID, DWORD); memcpy(&set, &f, sizeof f);
    const BOOL dark = TRUE; const COLORREF caption = C_BACKGROUND, border = C_BORDER;
    if (FAILED(set(window, 20, &dark, sizeof dark))) set(window, 19, &dark, sizeof dark); /* DWMWA_USE_IMMERSIVE_DARK_MODE */
    set(window, 35, &caption, sizeof caption); set(window, 34, &border, sizeof border); /* Windows 11: caption / border colour */
}
static void fill(HDC dc, RECT r, COLORREF c) { SetDCBrushColor(dc, c); FillRect(dc, &r, GetStockObject(DC_BRUSH)); }
static void rounded(HDC dc, RECT r, COLORREF face, COLORREF border, int width, int radius) {
    HPEN pen = CreatePen(PS_INSIDEFRAME, width, border); HBRUSH brush = CreateSolidBrush(face);
    HGDIOBJ old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, brush);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, old_pen); SelectObject(dc, old_brush); DeleteObject(pen); DeleteObject(brush);
}
/* The banner: a warm stadium glow at the top fading into the background. */
static COLORREF banner_color(int y) {
    const int h = sx(BANNER_H), t = y < 0 ? 0 : y > h ? h : y;
    return RGB(0x24 + (0x0B - 0x24) * t / h, 0x1D + (0x0B - 0x1D) * t / h, 0x12 + (0x0D - 0x12) * t / h);
}
/* <game>\sce_sys\icon0.png (or pic1.png) of the user's dump, scaled with WIC and laid over the
 * banner's colours once; nothing when the folder has neither. */
static void banner_load(const wchar_t *game) {
    if (banner_art) { DeleteObject(banner_art); banner_art = NULL; }
    static const wchar_t *const names[] = {L"sce_sys\\icon0.png", L"sce_sys\\pic1.png"};
    wchar_t path[PATHW]; int found = 0;
    for (int i = 0; i < COUNT(names) && game[0] && !found; ++i) { join(path, PATHW, game, names[i]); found = exists(path); }
    if (!found) return;
    IWICImagingFactory *factory = NULL; IWICBitmapDecoder *decoder = NULL; IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL; IWICFormatConverter *converter = NULL;
    unsigned char *pixels = NULL; UINT w = 0, h = 0, tw = 0, th = 0;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory))) return;
    if (SUCCEEDED(factory->lpVtbl->CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->lpVtbl->GetFrame(decoder, 0, &frame)) && SUCCEEDED(frame->lpVtbl->GetSize(frame, &w, &h)) && w && h) {
        th = (UINT)sx(BANNER_H - 24); tw = w * th / h;                         /* icon0: square; pic1: 16:9 */
        if (tw > (UINT)sx(190)) { tw = (UINT)sx(190); th = h * tw / w; }
        if (tw && th && SUCCEEDED(factory->lpVtbl->CreateBitmapScaler(factory, &scaler)) &&
            SUCCEEDED(scaler->lpVtbl->Initialize(scaler, (IWICBitmapSource *)frame, tw, th, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(factory->lpVtbl->CreateFormatConverter(factory, &converter)) &&
            SUCCEEDED(converter->lpVtbl->Initialize(converter, (IWICBitmapSource *)scaler, &GUID_WICPixelFormat32bppPBGRA,
                                                    WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) &&
            (pixels = malloc((size_t)tw * th * 4)) &&
            SUCCEEDED(converter->lpVtbl->CopyPixels(converter, NULL, tw * 4, tw * th * 4, pixels))) {
            BITMAPINFO info = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER), .biWidth = (LONG)tw, .biHeight = -(LONG)th, .biPlanes = 1,
                                             .biBitCount = 32, .biCompression = BI_RGB}};
            void *bits = NULL;
            banner_art = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
            if (banner_art && bits) {
                const int top = (sx(BANNER_H) - (int)th) / 2;
                unsigned char *out = bits;
                for (UINT y = 0; y < th; ++y) {
                    const COLORREF bg = banner_color(top + (int)y);
                    for (UINT x = 0; x < tw; ++x) {
                        const unsigned char *p = pixels + ((size_t)y * tw + x) * 4; unsigned char *o = out + ((size_t)y * tw + x) * 4;
                        const int a = 255 - p[3]; /* premultiplied over the banner */
                        o[0] = (unsigned char)(p[0] + GetBValue(bg) * a / 255); o[1] = (unsigned char)(p[1] + GetGValue(bg) * a / 255);
                        o[2] = (unsigned char)(p[2] + GetRValue(bg) * a / 255); o[3] = 255;
                    }
                }
                banner_art_w = (int)tw; banner_art_h = (int)th;
            } else if (banner_art) { DeleteObject(banner_art); banner_art = NULL; }
        }
    }
    free(pixels);
    if (converter) converter->lpVtbl->Release(converter);
    if (scaler) scaler->lpVtbl->Release(scaler);
    if (frame) frame->lpVtbl->Release(frame);
    if (decoder) decoder->lpVtbl->Release(decoder);
    factory->lpVtbl->Release(factory);
}
static void paint_window(HDC dc, RECT client) {
    fill(dc, client, C_BACKGROUND);
    SetBkMode(dc, TRANSPARENT);
    /* The banner. */
    const int banner_h = sx(BANNER_H);
    for (int y = 0; y < banner_h; ++y) fill(dc, (RECT){0, y, client.right, y + 1}, banner_color(y));
    int text_x = sx(24);
    if (banner_art) {
        const int top = (banner_h - banner_art_h) / 2;
        HDC art = CreateCompatibleDC(dc); HGDIOBJ old = SelectObject(art, banner_art);
        BitBlt(dc, sx(16), top, banner_art_w, banner_art_h, art, 0, 0, SRCCOPY);
        SelectObject(art, old); DeleteDC(art);
        SetDCPenColor(dc, C_GOLD); HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN)), old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, sx(16) - 1, top - 1, sx(16) + banner_art_w + 1, top + banner_art_h + 1);
        SelectObject(dc, old_pen); SelectObject(dc, old_brush);
        text_x = sx(16) + banner_art_w + sx(22);
    }
    RECT t = {text_x, sx(26), client.right - sx(16), sx(66)};
    SelectObject(dc, font_title); SetTextColor(dc, C_GOLD);
    DrawTextW(dc, L"EA SPORTS UFC 3 \x2014 native PC port", -1, &t, DT_SINGLELINE | DT_LEFT | DT_BOTTOM | DT_NOPREFIX | DT_END_ELLIPSIS);
    char version[64]; read_version(version, sizeof(version));
    wchar_t line[160]; _snwprintf(line, 160, L"PS4 CUSA06534 \x00B7 SacredFire %hs \x00B7 your own game dump required", version); line[159] = 0;
    t.top = sx(70); t.bottom = sx(90);
    SelectObject(dc, ui_font); SetTextColor(dc, C_TEXT_DIM);
    DrawTextW(dc, line, -1, &t, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
#ifdef UFC3_DIAGNOSTIC_PACKAGE
    t.top = sx(92); t.bottom = sx(112); SetTextColor(dc, C_RED);
    DrawTextW(dc, L"Windows diagnostic test: write windows + audit forced, data in diagnostic-data", -1, &t, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
#endif
    fill(dc, (RECT){0, banner_h - sx(2), client.right, banner_h}, C_GOLD);
    /* The sections: a panel, a gold bar and the title. */
    SelectObject(dc, font_section);
    for (int i = 0; i < COUNT(panels); ++i) {
        RECT r = {sx(panels[i].x), sx(panels[i].y), sx(panels[i].x + panels[i].w), sx(panels[i].y + panels[i].h)};
        rounded(dc, r, C_PANEL, C_BORDER, 1, sx(10));
        fill(dc, (RECT){r.left + sx(12), r.top + sx(10), r.left + sx(15), r.top + sx(26)}, C_GOLD);
        RECT title = {r.left + sx(22), r.top + sx(8), r.right - sx(12), r.top + sx(28)};
        SetTextColor(dc, C_GOLD);
        DrawTextW(dc, panels[i].title, -1, &title, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);
    }
}
/* Owner-drawn: Play (red, gold border), Stop (red outline), the other buttons (dark, gold on
 * hover) and the check boxes (gold when on). */
static void draw_button(const DRAWITEMSTRUCT *d) {
    const int id = (int)d->CtlID;
    HDC dc = d->hDC; RECT r = d->rcItem;
    const int enabled = !(d->itemState & ODS_DISABLED), pressed = (d->itemState & ODS_SELECTED) != 0, hot = enabled && hot_id == id;
    const int on_panel = id != ID_PLAY && id != ID_STOP && id != ID_STEAM;
    fill(dc, r, on_panel ? C_PANEL : C_BACKGROUND);
    wchar_t text[128]; GetWindowTextW(d->hwndItem, text, COUNT(text));
    SetBkMode(dc, TRANSPARENT);
    const int focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
    if (id >= 100 && id < ID_LAST && is_check[id - 100]) {
        const int on = check_state[id - 100], box = sx(16), top = r.top + (r.bottom - r.top - box) / 2;
        RECT b = {r.left + 1, top, r.left + 1 + box, top + box};
        rounded(dc, b, on ? (enabled ? C_GOLD : C_BORDER) : C_INPUT, !enabled ? C_BORDER : hot || on ? C_GOLD_LIGHT : HEX(0x4A4A52), 1, sx(4));
        if (on) {
            HPEN pen = CreatePen(PS_SOLID, sx(2), enabled ? C_BACKGROUND : C_TEXT_DIM); HGDIOBJ old = SelectObject(dc, pen);
            const POINT mark[] = {{b.left + sx(4), b.top + sx(8)}, {b.left + sx(7), b.bottom - sx(4)}, {b.right - sx(4), b.top + sx(4)}};
            Polyline(dc, mark, 3);
            SelectObject(dc, old); DeleteObject(pen);
        }
        RECT t = r; t.left = b.right + sx(8);
        SelectObject(dc, ui_font); SetTextColor(dc, !enabled ? C_TEXT_OFF : hot ? C_GOLD_LIGHT : C_TEXT);
        DrawTextW(dc, text, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (focus) {
            RECT f = t; DrawTextW(dc, text, -1, &f, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_CALCRECT);
            OffsetRect(&f, 0, (r.bottom - r.top - (f.bottom - f.top)) / 2 - (f.top - r.top)); InflateRect(&f, sx(2), 0); DrawFocusRect(dc, &f);
        }
        return;
    }
    COLORREF face, border, ink; int width = 1; HFONT font = ui_font;
    if (id == ID_PLAY) {
        face = !enabled ? HEX(0x3A1416) : pressed ? HEX(0xA00808) : hot ? HEX(0xE81C1C) : C_RED;
        border = !enabled ? C_BORDER : hot ? C_GOLD_LIGHT : C_GOLD; ink = enabled ? C_TEXT : C_TEXT_DIM; width = sx(2); font = font_action;
    } else if (id == ID_STOP) {
        face = !enabled ? C_BACKGROUND : pressed ? HEX(0x3A0A0A) : hot ? HEX(0x260C0C) : C_BACKGROUND;
        border = enabled ? C_RED : C_BORDER; ink = enabled ? HEX(0xFF4040) : C_TEXT_OFF; width = sx(2); font = font_action;
    } else {
        face = !enabled ? C_PANEL : pressed ? HEX(0x101014) : hot ? HEX(0x26262E) : C_INPUT;
        border = !enabled ? C_BORDER : hot ? C_GOLD_LIGHT : HEX(0x3A3A42); ink = !enabled ? C_TEXT_OFF : hot ? C_GOLD_LIGHT : C_TEXT;
    }
    rounded(dc, r, face, border, width, sx(8));
    SelectObject(dc, font); SetTextColor(dc, ink);
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    if (focus) { RECT f = r; InflateRect(&f, -sx(4), -sx(4)); DrawFocusRect(dc, &f); }
}
/* Hover for the owner-drawn buttons. */
static LRESULT CALLBACK hover_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data_unused) {
    (void)data_unused;
    if (message == WM_MOUSEMOVE && hot_id != (int)id) {
        const int previous = hot_id; hot_id = (int)id;
        TRACKMOUSEEVENT track = {.cbSize = sizeof(track), .dwFlags = TME_LEAVE, .hwndTrack = window};
        TrackMouseEvent(&track);
        if (previous >= 100 && previous < ID_LAST && control(previous)) InvalidateRect(control(previous), NULL, FALSE);
        InvalidateRect(window, NULL, FALSE);
    } else if (message == WM_MOUSELEAVE && hot_id == (int)id) { hot_id = 0; InvalidateRect(window, NULL, FALSE); }
    else if (message == WM_NCDESTROY) RemoveWindowSubclass(window, hover_proc, id);
    return DefSubclassProc(window, message, wparam, lparam);
}
/* The window's controls, at 96 DPI. */
static HWND label(const wchar_t *text, int x, int y, int w) { return make(L"STATIC", text, SS_CENTERIMAGE | SS_NOPREFIX, x, y, w, 24, 0); }
static HWND note(const wchar_t *text, int x, int y, int w, int h) {
    HWND n = make(L"STATIC", text, SS_NOPREFIX, x, y, w, h, 0);
    SendMessageW(n, WM_SETFONT, (WPARAM)font_small, TRUE);
    if (note_count < COUNT(notes)) notes[note_count++] = n;
    return n;
}
static HWND button(const wchar_t *text, int x, int y, int w, int h, int id) {
    HWND b = make(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id);
    SetWindowSubclass(b, hover_proc, (UINT_PTR)id, 0);
    return b;
}
static HWND checkbox(const wchar_t *text, int x, int y, int w, int id) { is_check[id - 100] = 1; return button(text, x, y, w, 22, id); }
static HWND combo(int x, int y, int w, int id, int editable) {
    HWND c = make(L"COMBOBOX", L"", (editable ? CBS_DROPDOWN | CBS_AUTOHSCROLL : CBS_DROPDOWNLIST) | WS_VSCROLL | WS_TABSTOP, x, y, w, 300, id);
    dark_theme(c, L"DarkMode_CFD");
    return c;
}
static HWND edit(const wchar_t *text, int x, int y, int w, int id) {
    HWND e = make(L"EDIT", text, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, x, y, w, 24, id);
    dark_theme(e, L"DarkMode_CFD");
    return e;
}
static void fill_gamepads(void) {
    HWND combo = control(ID_GAMEPAD);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Any gamepad (or the keyboard)");
    int selected = 0;
    for (int i = 0; i < gamepad_count; ++i) {
        wchar_t label[256]; _snwprintf(label, 256, L"%ls  [%.8ls]", gamepads[i].name, gamepads[i].guid); label[255] = 0;
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label);
        if (!wcscmp(gamepads[i].guid, settings.gamepad)) selected = i + 1;
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

/* ---- Controls: key.<input>= / pad.<input>= lines of bbport.ini (runtime_pad.c reads them) ---- */
enum { CTL_INPUTS = 25 };
static const char *const ctl_inputs[CTL_INPUTS] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "options", "touchpad",
    "touchpad_right", "up", "down", "left", "right", "move_up", "move_down", "move_left", "move_right",
    "look_up", "look_down", "look_left", "look_right"};
static const wchar_t *const ctl_labels[CTL_INPUTS] = {
    L"Cross (X)", L"Circle", L"Square", L"Triangle", L"L1", L"R1", L"L2", L"R2", L"L3 (left stick click)",
    L"R3 (right stick click)", L"Options", L"Touchpad (left half)", L"Touchpad (right half)", L"D-pad up",
    L"D-pad down", L"D-pad left", L"D-pad right", L"Left stick up", L"Left stick down", L"Left stick left",
    L"Left stick right", L"Right stick up", L"Right stick down", L"Right stick left", L"Right stick right"};
/* The runtime's defaults (runtime_pad.c bind_defaults), as SDL names. */
static const char *const ctl_default_keys[CTL_INPUTS] = {
    "Space", "Left Shift", "E", "Q", "1", "3", "R", "F", "Z", "C", "Return", "Tab", "Backspace",
    "I", "K", "J", "L", "W", "S", "A", "D", "Up", "Down", "Left", "Right"};
static const char *const ctl_default_pads[CTL_INPUTS] = {
    "a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "leftstick",
    "rightstick", "start", "back,touchpad", "", "dpup", "dpdown", "dpleft", "dpright"};
/* Gamepad buttons (SDL names) and what they are on Xbox / PlayStation pads. */
static const char *const ctl_pad_names[] = {
    "", "a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "leftstick",
    "rightstick", "start", "back", "guide", "touchpad", "dpup", "dpdown", "dpleft", "dpright", "misc1",
    "paddle1", "paddle2", "paddle3", "paddle4"};
static const wchar_t *const ctl_pad_labels[] = {
    L"(none)", L"A / Cross", L"B / Circle", L"X / Square", L"Y / Triangle", L"LB / L1", L"RB / R1",
    L"LT / L2", L"RT / R2", L"Left stick click", L"Right stick click", L"Start / Options", L"Back / Share",
    L"Guide / PS", L"Touchpad click", L"D-pad up", L"D-pad down", L"D-pad left", L"D-pad right",
    L"Misc (Share / Capture)", L"Paddle 1", L"Paddle 2", L"Paddle 3", L"Paddle 4"};
static char ctl_keys[CTL_INPUTS][64], ctl_pads[CTL_INPUTS][64];
static HWND ctl_window, ctl_key_buttons[CTL_INPUTS], ctl_pad_combos[CTL_INPUTS];
static int ctl_capture = -1; /* the row waiting for a key */
enum { ID_CTL_KEY = 2000, ID_CTL_PAD = 2100, ID_CTL_SAVE = 2200, ID_CTL_DEFAULTS, ID_CTL_CANCEL };

static void ctl_show_key(int row) {
    wchar_t text[96];
    if (ctl_capture == row) wcscpy(text, L"Press a key... (Esc: cancel, Delete: none)");
    else if (!ctl_keys[row][0]) wcscpy(text, L"(none)");
    else { MultiByteToWideChar(CP_UTF8, 0, ctl_keys[row], -1, text, 96); }
    SetWindowTextW(ctl_key_buttons[row], text);
}
static void ctl_show_pad(int row) {
    if (row >= 17) return;
    int selected = 0;
    char first[64]; snprintf(first, sizeof(first), "%s", ctl_pads[row]);
    char *comma = strchr(first, ','); if (comma) *comma = 0;
    for (int i = 0; i < COUNT(ctl_pad_names); ++i) if (!_stricmp(first, ctl_pad_names[i])) selected = i;
    SendMessageW(ctl_pad_combos[row], CB_SETCURSEL, selected, 0);
}
static void ctl_load(int defaults) {
    for (int i = 0; i < CTL_INPUTS; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "key.%s", ctl_inputs[i]);
        const char *k = defaults ? NULL : ini_value(key);
        snprintf(ctl_keys[i], sizeof(ctl_keys[i]), "%s", k ? k : ctl_default_keys[i]);
        snprintf(key, sizeof(key), "pad.%s", ctl_inputs[i]);
        const char *p = (defaults || i >= 17) ? NULL : ini_value(key);
        snprintf(ctl_pads[i], sizeof(ctl_pads[i]), "%s", p ? p : (i < 17 ? ctl_default_pads[i] : ""));
    }
}
static void ctl_save(void) {
    for (int i = 0; i < CTL_INPUTS; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "key.%s", ctl_inputs[i]); ini_set(key, ctl_keys[i]);
        if (i < 17) { snprintf(key, sizeof(key), "pad.%s", ctl_inputs[i]); ini_set(key, ctl_pads[i]); }
    }
    logw(L"Controls saved to bbport.ini\n");
}
/* A Windows key press as SDL's scancode name (SDL_GetScancodeFromName in runtime_pad.c). */
static int vk_to_sdl_name(WPARAM vk, LPARAM lparam, char *out, size_t n) {
    const int extended = (lparam >> 24) & 1, scan = (lparam >> 16) & 0xff;
    if (vk >= 'A' && vk <= 'Z') { snprintf(out, n, "%c", (int)vk); return 1; }
    if (vk >= '0' && vk <= '9') { snprintf(out, n, "%c", (int)vk); return 1; }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(out, n, "F%d", (int)(vk - VK_F1 + 1)); return 1; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(out, n, "Keypad %d", (int)(vk - VK_NUMPAD0)); return 1; }
    const char *name = NULL;
    switch (vk) {
    case VK_SPACE: name = "Space"; break;
    case VK_RETURN: name = extended ? "Keypad Enter" : "Return"; break;
    case VK_TAB: name = "Tab"; break;
    case VK_BACK: name = "Backspace"; break;
    case VK_SHIFT: name = scan == 0x36 ? "Right Shift" : "Left Shift"; break;
    case VK_CONTROL: name = extended ? "Right Ctrl" : "Left Ctrl"; break;
    case VK_MENU: name = extended ? "Right Alt" : "Left Alt"; break;
    case VK_UP: name = "Up"; break;
    case VK_DOWN: name = "Down"; break;
    case VK_LEFT: name = "Left"; break;
    case VK_RIGHT: name = "Right"; break;
    case VK_HOME: name = "Home"; break;
    case VK_END: name = "End"; break;
    case VK_PRIOR: name = "PageUp"; break;
    case VK_NEXT: name = "PageDown"; break;
    case VK_INSERT: name = "Insert"; break;
    case VK_CAPITAL: name = "CapsLock"; break;
    case VK_OEM_COMMA: name = ","; break;
    case VK_OEM_PERIOD: name = "."; break;
    case VK_OEM_MINUS: name = "-"; break;
    case VK_OEM_PLUS: name = "="; break;
    case VK_OEM_1: name = ";"; break;
    case VK_OEM_2: name = "/"; break;
    case VK_OEM_3: name = "`"; break;
    case VK_OEM_4: name = "["; break;
    case VK_OEM_5: name = "\\"; break;
    case VK_OEM_6: name = "]"; break;
    case VK_OEM_7: name = "'"; break;
    case VK_MULTIPLY: name = "Keypad *"; break;
    case VK_ADD: name = "Keypad +"; break;
    case VK_SUBTRACT: name = "Keypad -"; break;
    case VK_DIVIDE: name = "Keypad /"; break;
    case VK_DECIMAL: name = "Keypad ."; break;
    }
    if (!name) return 0;
    snprintf(out, n, "%s", name);
    return 1;
}
/* Called from the message loop: the key for the row waiting for one. */
static int ctl_capture_key(const MSG *msg) {
    if (ctl_capture < 0 || (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN)) return 0;
    const int row = ctl_capture;
    if (msg->wParam == VK_ESCAPE) { ctl_capture = -1; ctl_show_key(row); return 1; }
    if (msg->wParam == VK_DELETE) { ctl_keys[row][0] = 0; ctl_capture = -1; ctl_show_key(row); return 1; }
    char name[64];
    if (!vk_to_sdl_name(msg->wParam, msg->lParam, name, sizeof(name))) return 1; /* not bindable: keep waiting */
    for (int i = 0; i < CTL_INPUTS; ++i) /* one input per key: the old owner is cleared */
        if (i != row && !_stricmp(ctl_keys[i], name)) { ctl_keys[i][0] = 0; ctl_show_key(i); }
    snprintf(ctl_keys[row], sizeof(ctl_keys[row]), "%s", name);
    ctl_capture = -1;
    ctl_show_key(row);
    return 1;
}
static HWND ctl_make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND h_wnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, sx(x), sx(y), sx(w), sx(h), ctl_window,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h_wnd, WM_SETFONT, (WPARAM)ui_font, TRUE);
    return h_wnd;
}
static void ctl_refresh(void) { for (int i = 0; i < CTL_INPUTS; ++i) { ctl_show_key(i); ctl_show_pad(i); } }
static LRESULT CALLBACK ctl_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id >= ID_CTL_KEY && id < ID_CTL_KEY + CTL_INPUTS) {
            const int previous = ctl_capture;
            ctl_capture = id - ID_CTL_KEY;
            if (previous >= 0) ctl_show_key(previous);
            ctl_show_key(ctl_capture);
        } else if (id >= ID_CTL_PAD && id < ID_CTL_PAD + 17 && HIWORD(wparam) == CBN_SELCHANGE) {
            const int row = id - ID_CTL_PAD;
            const int sel = (int)SendMessageW(ctl_pad_combos[row], CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < COUNT(ctl_pad_names)) snprintf(ctl_pads[row], sizeof(ctl_pads[row]), "%s", ctl_pad_names[sel]);
        } else if (id == ID_CTL_DEFAULTS) { ctl_capture = -1; ctl_load(1); ctl_refresh(); }
        else if (id == ID_CTL_SAVE) { ctl_capture = -1; ctl_save(); DestroyWindow(window); }
        else if (id == ID_CTL_CANCEL) { ctl_capture = -1; DestroyWindow(window); }
        return 0;
    }
    case WM_CLOSE: ctl_capture = -1; DestroyWindow(window); return 0;
    case WM_DESTROY: ctl_window = NULL; EnableWindow(main_window, TRUE); SetForegroundWindow(main_window); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
static void controls_open(void) {
    if (ctl_window) { SetForegroundWindow(ctl_window); return; }
    static int registered;
    if (!registered) {
        WNDCLASSW wc = {.lpfnWndProc = ctl_proc, .hInstance = GetModuleHandleW(NULL), .hCursor = LoadCursorW(NULL, IDC_ARROW),
                        .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1), .lpszClassName = L"UFC3NativeControls"};
        RegisterClassW(&wc);
        registered = 1;
    }
    ctl_load(0);
    const int rows_h = 26, top = 40, client_w = 640, client_h = top + CTL_INPUTS * rows_h + 56;
    RECT r = {0, 0, sx(client_w), sx(client_h)};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    ctl_window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"UFC3NativeControls", L"Controls", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                 CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, main_window, NULL,
                                 GetModuleHandleW(NULL), NULL);
    if (!ctl_window) return;
    ctl_make(L"STATIC", L"Keyboard: click a key box, then press the key. Gamepad: pick a button. The keyboard and a "
                        L"gamepad work together.", 0, 12, 8, 616, 30, 0);
    for (int i = 0; i < CTL_INPUTS; ++i) {
        const int y = top + i * rows_h;
        ctl_make(L"STATIC", ctl_labels[i], SS_CENTERIMAGE, 12, y, 180, 22, 0);
        ctl_key_buttons[i] = ctl_make(L"BUTTON", L"", BS_PUSHBUTTON, 196, y, 230, 22, ID_CTL_KEY + i);
        if (i < 17) {
            ctl_pad_combos[i] = ctl_make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 436, y, 192, 300, ID_CTL_PAD + i);
            for (int k = 0; k < COUNT(ctl_pad_labels); ++k) SendMessageW(ctl_pad_combos[i], CB_ADDSTRING, 0, (LPARAM)ctl_pad_labels[k]);
        } else ctl_make(L"STATIC", L"(gamepad: the stick itself)", SS_CENTERIMAGE, 436, y, 192, 22, 0);
    }
    const int by = top + CTL_INPUTS * rows_h + 14;
    ctl_make(L"BUTTON", L"Defaults", BS_PUSHBUTTON, 12, by, 110, 30, ID_CTL_DEFAULTS);
    ctl_make(L"BUTTON", L"Save", BS_DEFPUSHBUTTON, 400, by, 110, 30, ID_CTL_SAVE);
    ctl_make(L"BUTTON", L"Cancel", BS_PUSHBUTTON, 518, by, 110, 30, ID_CTL_CANCEL);
    ctl_refresh();
    EnableWindow(main_window, FALSE);
    ShowWindow(ctl_window, SW_SHOW);
}

/* The window into the settings; patch switches go to bbport.ini when they changed. */
static void online_controls(void) {
    int mode = (int)SendMessageW(control(ID_MODE), CB_GETCURSEL, 0, 0);
    EnableWindow(control(ID_ONLINE), !running && mode == ONLINE_JOIN);
    EnableWindow(control(ID_HOST), !running && mode == ONLINE_HOST);
    EnableWindow(control(ID_HOSTSCAN), !running && mode == ONLINE_HOST);
    EnableWindow(control(ID_COPYHOST), mode == ONLINE_HOST);
}
/* 2K / 4K run in PS4 Pro mode: the box shows it on and locked; the user's own choice (neo_user)
 * comes back with 720p / 1080p and is what sacredfire.ini keeps. */
static int neo_user;
static void video_controls(void) {
    const int r = clampi((int)SendMessageW(control(ID_RESOLUTION), CB_GETCURSEL, 0, 0), COUNT(resolution_names));
    const int forced = resolution_experiment[r] != NULL;
    set_checked(ID_NEO, forced || neo_user);
    EnableWindow(control(ID_NEO), !running && !forced);
}
static void fill_host_addresses(void) {
    wchar_t current[256]; GetWindowTextW(control(ID_HOST), current, COUNT(current));
    if (!current[0]) wcsncpy(current, settings.host_address, COUNT(current)-1);
    local_addresses_refresh();
    SendMessageW(control(ID_HOST), CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < local_address_count; ++i)
        SendMessageW(control(ID_HOST), CB_ADDSTRING, 0, (LPARAM)local_addresses[i].label);
    if (current[0]) SetWindowTextW(control(ID_HOST), current);
    else if (local_address_count) SendMessageW(control(ID_HOST), CB_SETCURSEL, 0, 0);
}
static void copy_host_address(void) {
    wchar_t text[256], ip[64]; GetWindowTextW(control(ID_HOST), text, COUNT(text));
    address_token(text, ip, COUNT(ip));
    if (!ip[0] || !OpenClipboard(main_window)) return;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wcslen(ip)+1)*sizeof(wchar_t));
    if (memory) {
        void *dest = GlobalLock(memory);
        if (dest) {
            memcpy(dest, ip, (wcslen(ip)+1)*sizeof(wchar_t)); GlobalUnlock(memory);
            EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else GlobalFree(memory);
    }
    CloseClipboard();
}
static void read_controls(void) {
    GetWindowTextW(control(ID_GAME), settings.game, PATHW); trimw(settings.game);
    {
        /* The PS4 allows 16 characters; the game reads them as bytes: printable ASCII only. */
        wchar_t name[64]; GetWindowTextW(control(ID_PLAYER), name, 64); trimw(name);
        int k = 0;
        for (int i = 0; name[i] && k < 16; ++i) if (name[i] >= 0x20 && name[i] < 0x7f) settings.player[k++] = name[i];
        settings.player[k] = 0;
        SetWindowTextW(control(ID_PLAYER), settings.player);
    }
    settings.online_mode = clampi((int)SendMessageW(control(ID_MODE), CB_GETCURSEL, 0, 0), 3);
    GetWindowTextW(control(ID_ONLINE), settings.join_address, COUNT(settings.join_address));
    trimw(settings.join_address);
    wchar_t host[256]; GetWindowTextW(control(ID_HOST), host, COUNT(host));
    address_token(host, settings.host_address, COUNT(settings.host_address));
    int sel = (int)SendMessageW(control(ID_GAMEPAD), CB_GETCURSEL, 0, 0);
    if (sel > 0 && sel <= gamepad_count) wcsncpy(settings.gamepad, gamepads[sel - 1].guid, 127); else settings.gamepad[0] = 0;
    settings.fullscreen = checked(ID_FULLSCREEN); settings.neo = neo_user; settings.skip_movies = checked(ID_SKIPMOVIES); settings.clear_cache = checked(ID_CLEAR);
    settings.validation = checked(ID_VALIDATION); settings.frame_stats = checked(ID_STATS); settings.menu_trace = checked(ID_TRACE);
    settings.resolution = clampi((int)SendMessageW(control(ID_RESOLUTION), CB_GETCURSEL, 0, 0), COUNT(resolution_names));
    settings.fps_mode = clampi((int)SendMessageW(control(ID_FPS), CB_GETCURSEL, 0, 0), COUNT(fps_names));
    settings.present_mode = clampi((int)SendMessageW(control(ID_PRESENT), CB_GETCURSEL, 0, 0), COUNT(present_names));
    settings.upscaler = clampi((int)SendMessageW(control(ID_UPSCALER), CB_GETCURSEL, 0, 0), COUNT(upscaler_names));
    settings.sharpness = clampi((int)SendMessageW(control(ID_SHARPNESS), CB_GETCURSEL, 0, 0), COUNT(sharpness_names));
    settings.display = clampi((int)SendMessageW(control(ID_DISPLAY), CB_GETCURSEL, 0, 0), COUNT(display_names));
    settings.language = clampi((int)SendMessageW(control(ID_LANGUAGE), CB_GETCURSEL, 0, 0), COUNT(language_names));
    GetWindowTextW(control(ID_LOGDIR), settings.log_dir, PATHW); trimw(settings.log_dir);
    const int show_fps = checked(ID_SHOWFPS);
    ini_set("show_fps", show_fps ? "1" : "0"); /* written always: the game's own default is off */
    settings.show_fps = show_fps;
    settings_save();
}
static int pick_folder(const wchar_t *title, wchar_t *out_path) {
    int ok = 0;
    IFileOpenDialog *dialog = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog))) {
        DWORD options = 0;
        dialog->lpVtbl->GetOptions(dialog, &options);
        dialog->lpVtbl->SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->lpVtbl->SetTitle(dialog, title);
        if (SUCCEEDED(dialog->lpVtbl->Show(dialog, main_window))) {
            IShellItem *item = NULL;
            if (SUCCEEDED(dialog->lpVtbl->GetResult(dialog, &item))) {
                PWSTR chosen = NULL;
                if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &chosen))) {
                    wcsncpy(out_path, chosen, PATHW - 1); out_path[PATHW - 1] = 0; CoTaskMemFree(chosen); ok = 1;
                }
                item->lpVtbl->Release(item);
            }
        }
        dialog->lpVtbl->Release(dialog);
    }
    return ok;
}
/* <logs>\<time>.log for a run of the game; <logs>\launcher.log while the window is open, with
 * <logs> the effective log folder (effective_log_dir). A folder that cannot be written falls
 * back to <data>\logs with a visible note. */
static void open_log_file(const wchar_t *fixed_name) {
    wchar_t logs[PATHW], log_path[PATHW];
    effective_log_dir(logs); ensure_directory(logs);
    SYSTEMTIME now; GetLocalTime(&now);
    if (fixed_name) join(log_path, PATHW, logs, fixed_name);
    else _snwprintf(log_path, PATHW, L"%ls\\%04u%02u%02u_%02u%02u%02u.log", logs, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    log_path[PATHW - 1] = 0;
    if (log_file) fclose(log_file);
    log_file = _wfopen(log_path, fixed_name ? L"ab" : L"wb");
    if (!log_file) {
        wchar_t fallback[PATHW]; join(fallback, PATHW, data, L"logs");
        if (wcscmp(fallback, logs)) {
            logw(L"Log folder \"%ls\" is not writable; ", logs);
            ensure_directory(fallback);
            if (fixed_name) join(log_path, PATHW, fallback, fixed_name);
            else _snwprintf(log_path, PATHW, L"%ls\\%04u%02u%02u_%02u%02u%02u.log", fallback, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
            log_path[PATHW - 1] = 0;
            log_file = _wfopen(log_path, fixed_name ? L"ab" : L"wb");
            if (log_file) logw(L"the log goes to %ls instead.\n", log_path);
            else logw(L"the fallback %ls is not writable either.\n", log_path);
        }
    }
    logw(L"Log: %ls\n", log_file ? log_path : L"(not writable)");
}
static const wchar_t *const *play_extra; static int play_extra_count;
static DWORD WINAPI play_thread(LPVOID p) {
    (void)p;
    const DWORD code = play(play_extra, play_extra_count);
    PostMessageW(main_window, WM_APP_DONE, code, 0);
    return 0;
}
static void set_running(int on) {
    running = on;
    EnableWindow(control(ID_PLAY), !on); EnableWindow(control(ID_STOP), on);
    for (int id = ID_GAME; id <= ID_LOGDIR_BROWSE; ++id) EnableWindow(control(id), !on);
    EnableWindow(control(ID_STEAM), !on);
    online_controls();
    video_controls();
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND: {
        const int id = LOWORD(wparam), code = HIWORD(wparam);
        /* Owner-drawn check boxes toggle themselves (a quick second click arrives as a double click). */
        if (id >= 100 && id < ID_LAST && is_check[id - 100]) {
            if (code == BN_CLICKED || code == BN_DOUBLECLICKED) { set_checked(id, !checked(id)); if (id == ID_NEO) neo_user = checked(id); }
            return 0;
        }
        if (code == BN_DOUBLECLICKED) return 0;
        if (id == ID_RESOLUTION && code == CBN_SELCHANGE) video_controls();
        else if (id == ID_GAME && code == EN_KILLFOCUS) {
            wchar_t folder[PATHW]; GetWindowTextW(control(ID_GAME), folder, PATHW); trimw(folder);
            banner_load(folder); RECT banner = {0, 0, sx(CLIENT_W), sx(BANNER_H)}; InvalidateRect(window, &banner, FALSE);
        }
        else if (id == ID_MODE && code == CBN_SELCHANGE) online_controls();
        else if (id == ID_HOSTSCAN && !running) fill_host_addresses();
        else if (id == ID_COPYHOST) copy_host_address();
        else if (id == ID_BROWSE) { wchar_t folder[PATHW]; if (pick_folder(L"The game folder (CUSA06534; CUSA06534-patch beside it is merged automatically)", folder)) {
            SetWindowTextW(control(ID_GAME), folder);
            banner_load(folder); RECT banner = {0, 0, sx(CLIENT_W), sx(BANNER_H)}; InvalidateRect(window, &banner, FALSE);
        } }
        else if (id == ID_LOGDIR_BROWSE && !running) {
            wchar_t folder[PATHW];
            if (pick_folder(L"The log folder (empty: the data folder's logs)", folder)) {
                SetWindowTextW(control(ID_LOGDIR), folder);
                wcsncpy(settings.log_dir, folder, PATHW - 1); settings.log_dir[PATHW - 1] = 0;
                settings_save();
                open_log_file(L"launcher.log"); /* the log follows the new folder at once */
            }
        }
        else if (id == ID_RESCAN) { gamepads_refresh(); fill_gamepads(); logw(L"Controllers: %d found\n", gamepad_count); }
        else if (id == ID_CONTROLS && !running) controls_open();
        else if (id == ID_PLAY && !running) {
            read_controls();
            open_log_file(NULL);
            InterlockedExchange(&stop_requested, 0);
            set_running(1);
            HANDLE t = CreateThread(NULL, 0, play_thread, NULL, 0, NULL);
            if (t) CloseHandle(t); else { fail(L"CreateThread failed"); set_running(0); }
        } else if (id == ID_STOP && running) {
            InterlockedExchange(&stop_requested, 1); server_kill();
            if (game_process) { logw(L"Stop: the game process is ended\n"); TerminateProcess(game_process, 1); }
        } else if (id == ID_STEAM && !running) {
            read_controls(); /* the shortcut uses the saved settings */
            add_to_steam();
        } else if (id == ID_LOGS || id == ID_DATA) {
            wchar_t folder[PATHW];
            if (id == ID_LOGS) effective_log_dir(folder); else { wcsncpy(folder, data, PATHW - 1); folder[PATHW - 1] = 0; }
            ensure_directory(folder);
            ShellExecuteW(window, L"open", folder, NULL, NULL, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_APP_LOG: {
        wchar_t *text = (wchar_t *)lparam;
        /* Edit controls want \r\n. */
        size_t n = wcslen(text), extra = 0;
        for (size_t i = 0; i < n; ++i) if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) ++extra;
        wchar_t *crlf = malloc((n + extra + 1) * sizeof(wchar_t));
        if (crlf) {
            size_t k = 0;
            for (size_t i = 0; i < n; ++i) { if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) crlf[k++] = L'\r'; crlf[k++] = text[i]; }
            crlf[k] = 0;
            if (GetWindowTextLengthW(log_edit) > 400000) SetWindowTextW(log_edit, L"(older lines are in the log file)\r\n");
            SendMessageW(log_edit, EM_SETSEL, (WPARAM)-1, -1);
            SendMessageW(log_edit, EM_REPLACESEL, FALSE, (LPARAM)crlf);
            free(crlf);
        }
        free(text);
        return 0;
    }
    case WM_ERASEBKGND: return 1; /* WM_PAINT draws everything (double buffered) */
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC screen = BeginPaint(window, &ps);
        RECT client; GetClientRect(window, &client);
        HDC dc = CreateCompatibleDC(screen); HBITMAP buffer = CreateCompatibleBitmap(screen, client.right, client.bottom);
        HGDIOBJ old = SelectObject(dc, buffer);
        paint_window(dc, client);
        BitBlt(screen, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old); DeleteObject(buffer); DeleteDC(dc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_DRAWITEM: draw_button((const DRAWITEMSTRUCT *)lparam); return TRUE;
    case WM_CTLCOLORBTN: return (LRESULT)brush_panel;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wparam; HWND child = (HWND)lparam;
        if (child == log_edit) { SetTextColor(dc, HEX(0xD4D4DA)); SetBkColor(dc, C_LOG); return (LRESULT)brush_log; }
        wchar_t cls[16] = L""; GetClassNameW(child, cls, COUNT(cls));
        /* Edits, lists and combo boxes (a disabled or read-only edit asks with WM_CTLCOLORSTATIC). */
        if (message != WM_CTLCOLORSTATIC || !_wcsicmp(cls, L"Edit") || !_wcsicmp(cls, L"ComboBox")) {
            SetTextColor(dc, IsWindowEnabled(child) ? C_TEXT : C_TEXT_DIM); SetBkColor(dc, C_INPUT); return (LRESULT)brush_input;
        }
        int is_note = 0;
        for (int i = 0; i < note_count; ++i) is_note |= notes[i] == child;
        SetTextColor(dc, is_note ? C_TEXT_DIM : C_TEXT); SetBkColor(dc, C_PANEL); return (LRESULT)brush_panel;
    }
    case WM_APP_DONE:
        set_running(0);
        open_log_file(L"launcher.log");
        return 0;
    case WM_CLOSE:
        if (running) {
            if (MessageBoxW(window, L"The game is running. Close it too?", L"SacredFire", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            InterlockedExchange(&stop_requested, 1); server_kill();
            if (game_process) TerminateProcess(game_process, 1);
        }
        read_controls();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
static int gui(HINSTANCE instance, const wchar_t *const *extra, int extra_count) {
    play_extra = extra; play_extra_count = extra_count;
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    { const UINT dpi = GetDpiForSystem(); scale = (dpi >= 96 && dpi <= 480) ? dpi / 96.0f : 1.0f; }
    ui_font = make_font(13, FW_NORMAL, L"Segoe UI");
    if (!ui_font) ui_font = GetStockObject(DEFAULT_GUI_FONT);
    font_section = make_font(13, FW_BOLD, L"Segoe UI"); font_title = make_font(28, FW_BOLD, L"Segoe UI");
    font_small = make_font(12, FW_NORMAL, L"Segoe UI"); font_action = make_font(16, FW_BOLD, L"Segoe UI");
    font_log = make_font(12, FW_NORMAL, L"Consolas");
    brush_background = CreateSolidBrush(C_BACKGROUND); brush_panel = CreateSolidBrush(C_PANEL);
    brush_input = CreateSolidBrush(C_INPUT); brush_log = CreateSolidBrush(C_LOG);
    WNDCLASSW wc = {.lpfnWndProc = window_proc, .hInstance = instance, .hCursor = LoadCursorW(NULL, IDC_ARROW),
                    .hbrBackground = brush_background, .lpszClassName = L"UFC3NativeLauncher", .hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1))};
    RegisterClassW(&wc);
    const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;
    /* The client area is CLIENT_W x CLIENT_H at 96 DPI: the window is created, measured, and
     * resized by the difference (the frame metrics of the running system, whatever they are). */
    const int client_w = sx(CLIENT_W), client_h = sx(CLIENT_H);
#if defined(UFC3_WRITE_WINDOWS_TEST)
    const wchar_t *window_title = L"EA SPORTS UFC 3 - native PC port (UFC-48 write windows test)";
#elif defined(UFC3_DIAGNOSTIC_PACKAGE)
    const wchar_t *window_title = L"EA SPORTS UFC 3 - native PC port (Windows diagnostic test)";
#else
    const wchar_t *window_title = L"EA SPORTS UFC 3 - native PC port";
#endif
    main_window = CreateWindowExW(0, wc.lpszClassName, window_title, style, CW_USEDEFAULT, CW_USEDEFAULT, client_w + 16, client_h + 64,
                                  NULL, NULL, instance, NULL);
    if (!main_window) return 1;
    theme_load(main_window);
    banner_load(settings.game);
    open_log_file(L"launcher.log");
    {
        RECT window_rect, client_rect;
        GetWindowRect(main_window, &window_rect); GetClientRect(main_window, &client_rect);
        const int dw = (window_rect.right - window_rect.left) - client_rect.right, dh = (window_rect.bottom - window_rect.top) - client_rect.bottom;
        if (dw >= 0 && dw < 200 && dh >= 0 && dh < 200) SetWindowPos(main_window, NULL, 0, 0, client_w + dw, client_h + dh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        GetWindowRect(main_window, &window_rect); GetClientRect(main_window, &client_rect);
        logw(L"Window: %ld,%ld %ldx%ld, client %ldx%ld, frame +%d+%d, DPI %u, caption %d\n", window_rect.left, window_rect.top,
             window_rect.right - window_rect.left, window_rect.bottom - window_rect.top, client_rect.right, client_rect.bottom, dw, dh,
             GetDpiForWindow(main_window), GetSystemMetrics(SM_CYCAPTION));
    }
    /* GAME (left column; content x 28..430) */
    int y = 134 + 34;
    label(L"Game folder (CUSA06534, with CUSA06534-patch beside it)", 28, y, 402); y += 26;
    edit(settings.game, 28, y, 306, ID_GAME);
    button(L"Browse...", 342, y, 88, 24, ID_BROWSE); y += 32;
    label(L"Player name", 28, y, 118);
    edit(settings.player[0] ? settings.player : L"Player 1", 150, y, 280, ID_PLAYER);
    SendMessageW(control(ID_PLAYER), EM_SETLIMITTEXT, 16, 0); y += 30;
    label(L"Language", 28, y, 118); combo(150, y, 280, ID_LANGUAGE, 0); y += 30;
    label(L"Controller", 28, y, 118); combo(150, y, 280, ID_GAMEPAD, 0); y += 30;
    note(L"In-game menu: L3+R3 or Insert", 28, y + 4, 190, 18);
    button(L"Rescan", 226, y, 90, 24, ID_RESCAN);
    button(L"Controls...", 322, y, 108, 24, ID_CONTROLS);
    /* ONLINE */
    y = 360 + 34;
    label(L"Online play", 28, y, 118); combo(150, y, 280, ID_MODE, 0); y += 30;
    label(L"Join address", 28, y, 118); edit(settings.join_address, 150, y, 280, ID_ONLINE); y += 30;
    label(L"Host address", 28, y, 118); combo(150, y, 280, ID_HOST, 1); y += 30;
    button(L"Rescan", 150, y, 90, 24, ID_HOSTSCAN);
    button(L"Copy IP", 246, y, 100, 24, ID_COPYHOST); y += 32;
    note(L"Host: share this PC's address with your friend (Copy IP). Outside your LAN both PCs need a VPN: "
         L"choose this PC's VPN IP. The server starts with Play and stops with the game.", 28, y, 402, 58);
    /* VIDEO (right column; content x 470..872) */
    y = 134 + 34;
    const struct { const wchar_t *text; int id; } video_rows[] = {
        {L"Game resolution", ID_RESOLUTION}, {L"Display resolution", ID_DISPLAY}, {L"Present mode", ID_PRESENT},
        {L"Upscaling", ID_UPSCALER}, {L"Sharpening (RCAS)", ID_SHARPNESS}, {L"FPS limit", ID_FPS}};
    for (int i = 0; i < COUNT(video_rows); ++i, y += 30) {
        label(video_rows[i].text, 470, y, 130); combo(604, y, 268, video_rows[i].id, 0);
        SendMessageW(control(video_rows[i].id), CB_SETDROPPEDWIDTH, (WPARAM)sx(330), 0);
    }
    y += 4;
    checkbox(L"Fullscreen", 470, y, 190, ID_FULLSCREEN); checkbox(L"PS4 Pro mode (sharper)", 672, y, 200, ID_NEO); y += 26;
    checkbox(L"Show the FPS counter", 470, y, 190, ID_SHOWFPS); checkbox(L"Skip the intro videos", 672, y, 200, ID_SKIPMOVIES); y += 30;
    note(L"2K / 4K (experimental): PS4 Pro with a 4K output; in game choose VIDEO \x2192 Favor Resolution. "
         L"720p / 1080p on a large screen: Display resolution with Upscaling = AMD FSR 1.0.", 470, y, 402, 50);
    /* ADVANCED */
    y = 476 + 40;
    checkbox(L"Clear the shader cache at start", 470, y, 210, ID_CLEAR); checkbox(L"Frame statistics", 692, y, 180, ID_STATS); y += 28;
    checkbox(L"Vulkan validation layer", 470, y, 210, ID_VALIDATION); checkbox(L"Menu input trace", 692, y, 180, ID_TRACE);
    /* LOGS (full width) */
    y = 592 + 34;
    label(L"Log folder", 28, y, 90);
    edit(settings.log_dir, 122, y, 440, ID_LOGDIR);
    button(L"Browse...", 570, y, 88, 24, ID_LOGDIR_BROWSE);
    button(L"Open logs", 666, y, 100, 24, ID_LOGS);
    button(L"Open data", 772, y, 100, 24, ID_DATA); y += 28;
    note(L"Empty: the data folder's logs. BB_LOG_DIR wins over it; a shared or network drive works.", 122, y, 750, 18);
    /* Play / Stop / Steam, then the log of the run. */
    y = 692;
    button(L"PLAY", 16, y, 170, 40, ID_PLAY);
    button(L"STOP", 196, y, 110, 40, ID_STOP);
    button(L"Add to Steam", 744, y, 140, 40, ID_STEAM); y += 52;
    log_edit = make(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 16, y, 868, CLIENT_H - y - 16, ID_LOG);
    SendMessageW(log_edit, WM_SETFONT, (WPARAM)font_log, TRUE);
    SendMessageW(log_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(sx(6), sx(6)));
    dark_theme(log_edit, L"DarkMode_Explorer");
    SendMessageW(log_edit, EM_SETLIMITTEXT, 0, 0);
    {
        const struct { int id, count, selected; const wchar_t *const *names; } combos[] = {
            {ID_RESOLUTION, COUNT(resolution_names), settings.resolution, resolution_names},
            {ID_DISPLAY, COUNT(display_names), settings.display, display_names},
            {ID_FPS, COUNT(fps_names), settings.fps_mode, fps_names},
            {ID_PRESENT, COUNT(present_names), settings.present_mode, present_names},
            {ID_UPSCALER, COUNT(upscaler_names), settings.upscaler, upscaler_names},
            {ID_SHARPNESS, COUNT(sharpness_names), settings.sharpness, sharpness_names},
            {ID_MODE, COUNT(online_mode_names), settings.online_mode, online_mode_names},
            {ID_LANGUAGE, COUNT(language_names), settings.language, language_names},
        };
        for (int c = 0; c < COUNT(combos); ++c) {
            for (int i = 0; i < combos[c].count; ++i) SendMessageW(control(combos[c].id), CB_ADDSTRING, 0, (LPARAM)combos[c].names[i]);
            SendMessageW(control(combos[c].id), CB_SETCURSEL, combos[c].selected, 0);
        }
    }
    neo_user = settings.neo;
    set_checked(ID_SHOWFPS, settings.show_fps); set_checked(ID_FULLSCREEN, settings.fullscreen); set_checked(ID_NEO, settings.neo); set_checked(ID_SKIPMOVIES, settings.skip_movies); set_checked(ID_CLEAR, settings.clear_cache);
    set_checked(ID_STATS, settings.frame_stats); set_checked(ID_VALIDATION, settings.validation); set_checked(ID_TRACE, settings.menu_trace);
    gamepads_refresh(); fill_gamepads(); fill_host_addresses();
    set_running(0);
    char version[64]; read_version(version, sizeof(version));
    logf("SacredFire %s (Windows). Data: ", version); logw(L"%ls\n", data);
#ifdef UFC3_DIAGNOSTIC_PACKAGE
    logw(L"Windows diagnostic test: the data stays beside the exe in diagnostic-data; write windows and their audit are on for every run.\n");
#endif
    logw(L"Controllers: %d found; window scale %d%%\n", gamepad_count, (int)(scale * 100));
    ShowWindow(main_window, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (ctl_capture_key(&msg)) continue;
        if (ctl_window && IsDialogMessageW(ctl_window, &msg)) continue;
        if (!IsDialogMessageW(main_window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous; (void)command_line; (void)show;
    /* Started from a console (cmd, a script): its output stays there as well. */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) console_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    /* A child that cannot load (0xC0000022 ...) reports to the log, not a system dialog. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    managed_env_capture();
    net_load();
    GetModuleFileNameW(NULL, root, PATHW);
    { wchar_t *slash = wcsrchr(root, L'\\'); if (slash) *slash = 0; }
    SetCurrentDirectoryW(root);
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int first = 1, play_now = 0;
    if (argc >= 3 && !wcscmp(argv[1], L"--after")) { /* an in-game restart waits for the previous game process (its GPU device) */
        HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)wcstoul(argv[2], NULL, 10));
        if (old) { WaitForSingleObject(old, 30000); CloseHandle(old); }
        first = 3;
    }
    const wchar_t *game_argument = NULL, *check_host = NULL;
    static const wchar_t *extra[64]; int extra_count = 0;
    for (int i = first; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--game") && i + 1 < argc) game_argument = argv[++i];
        else if (!wcscmp(argv[i], L"--play")) play_now = 1;
        else if (!wcscmp(argv[i], L"--check-host") && i + 1 < argc) check_host = argv[++i];
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            logw(L"SacredFire.exe [--play] [--game <folder>] [game options]\n"
                 L"  Without --play: the launcher window. --play starts the game with the saved settings.\n"
                 L"  BB_DATA_DIR, BB_GAME_DIR, BB_LOG_DIR, BB_TIMEOUT and the other variables of run.sh apply.\n");
            return 0;
        } else if (extra_count < 64) extra[extra_count++] = argv[i];
    }
    const wchar_t *data_env = env_or(L"BB_DATA_DIR", NULL);
    if (data_env) { if (!GetFullPathNameW(data_env, PATHW, data, NULL)) { fail(L"BB_DATA_DIR is not a usable path"); return 2; } }
#ifdef UFC3_DIAGNOSTIC_PACKAGE
    else join(data, PATHW, root, L"diagnostic-data"); /* the diagnostic run keeps its data beside the exe */
#else
    else {
        PWSTR local = NULL;
        if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local))) { fail(L"no %LOCALAPPDATA%"); return 2; }
        join(data, PATHW, local, L"UFC3Native"); CoTaskMemFree(local);
    }
#endif
    ensure_directory(data);
    join(python, PATHW, root, L"python\\python.exe");
    join(game_exe, PATHW, root, L"bin\\sacredfire-game.exe");
    join(caps_exe, PATHW, root, L"bin\\bb-gpu-capabilities.exe");
    settings_load();
    patches_load();
    if (check_host) {
        open_log_file(L"host-check.log");
        uint32_t ip; wchar_t message[1024];
        int result = 2;
        if (host_address_check(check_host, &ip, message, COUNT(message))) result = server_start(ip);
        else fail(message);
        server_stop(L"the host check finished");
        if (log_file) fclose(log_file);
        return result;
    }
    if (game_argument) { wcsncpy(settings.game, game_argument, PATHW - 1); settings.game[PATHW - 1] = 0; }
    else if (env_or(L"BB_GAME_DIR", NULL)) { wcsncpy(settings.game, env_or(L"BB_GAME_DIR", L""), PATHW - 1); settings.game[PATHW - 1] = 0; }
    if (play_now) {
        open_log_file(NULL);
        if (!settings.game[0]) { fail(L"No game folder: start the launcher once to choose it, or use --game <folder>."); return 2; }
        settings_save();
        const DWORD code = play(extra, extra_count);
        if (log_file) fclose(log_file);
        return (int)code;
    }
    return gui(instance, extra, extra_count);
}
