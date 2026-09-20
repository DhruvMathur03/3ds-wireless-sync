/*
 * 3DS Wireless Sync - directory browser / file sender
 *
 * Graphical rewrite: uses citro2d/citro3d instead of the text console.
 * Top screen  -> file/folder list with icons, highlight bar, scrollbar.
 * Bottom screen -> connection status, last action, control legend.
 *
 * Networking/browsing logic is unchanged from the original; only the
 * rendering layer was replaced. See the notes at the bottom of this
 * file for a summary of what changed and why.
 */

#include <citro2d.h>
#include <citro3d.h>
#include <3ds.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <stdbool.h>
#include <ctype.h>
#include "image_view.h"

/* ---------------- network / data config ---------------- */
#define PORT            8080
#define BUF_SIZE        1024
#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000
#define MAX_ITEMS       256
#define NAME_LEN        96
#define DISPLAY_NAME_MAX 40
#define ROWS_VISIBLE    11

/* ---------------- top screen layout (400x240) ---------------- */
#define TOP_W        400
#define HEADER_H     24
#define PATH_H       18
#define LIST_TOP     (HEADER_H + PATH_H)
#define ROW_H        18
#define SCROLLBAR_W  6
#define LIST_W       (TOP_W - SCROLLBAR_W)

/* ---------------- bottom screen layout (320x240) --------------- */
#define BOT_W         320
#define BOT_HEADER_H  24

/* ---------------- palette ---------------- */
#define COL_BG_TOP      C2D_Color32(0x1b, 0x1d, 0x2b, 0xff)
#define COL_BG_BOTTOM   C2D_Color32(0x15, 0x16, 0x22, 0xff)
#define COL_HEADER      C2D_Color32(0x2f, 0x5f, 0x8f, 0xff)
#define COL_PATHBAR     C2D_Color32(0x24, 0x26, 0x38, 0xff)
#define COL_ROW_HILITE  C2D_Color32(0x2c, 0x4f, 0x70, 0xff)
#define COL_ACCENT      C2D_Color32(0x5b, 0xb7, 0xe0, 0xff)
#define COL_TEXT        C2D_Color32(0xf0, 0xf0, 0xf5, 0xff)
#define COL_TEXT_DIM    C2D_Color32(0x9a, 0x9d, 0xb0, 0xff)
#define COL_FOLDER      C2D_Color32(0xe8, 0xb8, 0x4f, 0xff)
#define COL_FOLDER_TAB  C2D_Color32(0xc9, 0x9c, 0x3c, 0xff)
#define COL_FILE        C2D_Color32(0xd8, 0xda, 0xe4, 0xff)
#define COL_FILE_FOLD   C2D_Color32(0xb0, 0xb3, 0xc0, 0xff)
#define COL_FILE_LINE   C2D_Color32(0x8a, 0x8d, 0x9a, 0xff)
#define COL_SCROLL_TRACK C2D_Color32(0x24, 0x26, 0x38, 0xff)
#define COL_SCROLL_THUMB C2D_Color32(0x5b, 0xb7, 0xe0, 0xff)
#define COL_OK          C2D_Color32(0x4c, 0xd9, 0x7a, 0xff)
#define COL_ERR         C2D_Color32(0xe0, 0x5b, 0x5b, 0xff)
#define COL_WAIT        C2D_Color32(0xd9, 0xb8, 0x4c, 0xff)

/* ---------------- app state ---------------- */
typedef struct { char name[NAME_LEN]; bool isDir; } DirEntry;

static DirEntry items[MAX_ITEMS];
static int item_cnt = 0;
static int cursor = 0;
static int viewStart = 0;
static char cwd[256] = "sdmc:/";

static C2D_TextBuf g_dynBuf;
static char g_statusMsg[160] = "Starting...";
static u32  g_statusColor; /* set for real by set_status() before the first frame draws */
static bool viewingImage = false;

static bool has_ext(const char *name, const char *ext) {
    size_t nl = strlen(name), el = strlen(ext);
    if (nl < el) return false;
    for (size_t i = 0; i < el; i++)
        if (tolower((unsigned char)name[nl - el + i]) != tolower((unsigned char)ext[i]))
            return false;
    return true;
}

static bool is_jpg(const char *name) {
    return has_ext(name, ".jpg") || has_ext(name, ".jpeg");
}

/* Scans from `cursor` in the given direction (+1 or -1) for the next
 * entry that's a viewable image, skipping folders and other file
 * types. Returns its index, or -1 if there isn't one that way. */
static int find_next_image(int dir) {
    int idx = cursor + dir;
    while (idx >= 0 && idx < item_cnt) {
        if (!items[idx].isDir && is_jpg(items[idx].name)) return idx;
        idx += dir;
    }
    return -1;
}

/* ---------------- small helpers ---------------- */

static void set_status(const char *msg, u32 color) {
    strncpy(g_statusMsg, msg, sizeof(g_statusMsg) - 1);
    g_statusMsg[sizeof(g_statusMsg) - 1] = '\0';
    g_statusColor = color;
}

/* Truncates long filenames with an ellipsis so they never overflow
 * into the scrollbar / off the right edge of the screen. */
static void fit_name(const char *in, char *out, size_t outSize) {
    size_t cap = outSize - 1;
    if (cap > DISPLAY_NAME_MAX) cap = DISPLAY_NAME_MAX;
    size_t len = strlen(in);
    if (len <= cap) {
        strcpy(out, in);
        return;
    }
    size_t keep = cap > 3 ? cap - 3 : 0;
    memcpy(out, in, keep);
    out[keep] = '\0';
    strcat(out, "...");
}

static void draw_text(const char *str, float x, float y, float scale, u32 color) {
    C2D_Text text;
    C2D_TextParse(&text, g_dynBuf, str);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

static void draw_folder_icon(float x, float y) {
    C2D_DrawRectSolid(x, y, 0.5f, 7, 3, COL_FOLDER_TAB);
    C2D_DrawRectSolid(x, y + 3, 0.5f, 14, 10, COL_FOLDER);
}

static void draw_file_icon(float x, float y) {
    C2D_DrawRectSolid(x, y, 0.5f, 12, 13, COL_FILE);
    C2D_DrawTriangle(x + 8, y, COL_FILE_FOLD,
                      x + 12, y, COL_FILE_FOLD,
                      x + 12, y + 4, COL_FILE_FOLD, 0.5f);
    C2D_DrawRectSolid(x + 2, y + 6, 0.5f, 8, 1, COL_FILE_LINE);
    C2D_DrawRectSolid(x + 2, y + 9, 0.5f, 8, 1, COL_FILE_LINE);
}

static void draw_control_hint(float x, float y, const char *key, u32 color, const char *label) {
    float w = strlen(key) > 1 ? 34 : 18;
    C2D_DrawRectSolid(x, y, 0.5f, w, 16, color);
    draw_text(key, x + 3, y + 2, 0.36f, COL_BG_BOTTOM);
    draw_text(label, x + w + 6, y + 2, 0.36f, COL_TEXT_DIM);
}

/* ---------------- directory browsing ---------------- */

static void refresh_dir(void) {
    item_cnt = 0;
    viewStart = 0;
    cursor = 0;

    DIR *d = opendir(cwd);
    if (!d) {
        set_status("Could not open folder", COL_ERR);
        return;
    }

    if (strcmp(cwd, "sdmc:/") != 0) {
        strcpy(items[item_cnt].name, "..");
        items[item_cnt].isDir = true;
        ++item_cnt;
    }

    struct dirent *e;
    while ((e = readdir(d)) && item_cnt < MAX_ITEMS) {
        if (e->d_name[0] == '.') continue;
        strncpy(items[item_cnt].name, e->d_name, NAME_LEN - 1);
        items[item_cnt].name[NAME_LEN - 1] = '\0';
        items[item_cnt].isDir = (e->d_type == DT_DIR);
        ++item_cnt;
    }
    closedir(d);
}

static void go_up(void) {
    size_t len = strlen(cwd);
    if (len > 0 && cwd[len - 1] == '/') cwd[len - 1] = '\0';
    char *slash = strrchr(cwd, '/');
    if (slash) slash[1] = '\0';
}

/* ---------------- networking ---------------- */

static int start_server(void) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server);
        return -1;
    }

    listen(server, 4);
    int flags = fcntl(server, F_GETFL, 0);
    fcntl(server, F_SETFL, flags | O_NONBLOCK);
    return server;
}

static bool send_all(int sock, const void *buf, size_t len) {
    const u8 *p = buf;
    while (len) {
        int n = send(sock, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static void send_selected_file(int sock) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s", cwd, items[cursor].name);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        send(sock, "ERROR\n", 6, 0);
        set_status("Error: could not open file", COL_ERR);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "FILE %s %ld\n", items[cursor].name, size);

    bool ok = send_all(sock, hdr, strlen(hdr));
    char buf[BUF_SIZE];
    size_t r;
    while (ok && (r = fread(buf, 1, sizeof(buf), fp)) > 0)
        ok = send_all(sock, buf, r);
    fclose(fp);

    if (ok) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Sent %s (%ld bytes)", items[cursor].name, size);
        set_status(msg, COL_OK);
    } else {
        set_status("Transfer aborted (network error)", COL_ERR);
    }
}

/* ---------------- rendering ---------------- */

static void draw_top_screen(void) {
    C2D_DrawRectSolid(0, 0, 0.5f, TOP_W, HEADER_H, COL_HEADER);
    draw_text("3DS Wireless Sync", 8, 5, 0.5f, COL_TEXT);

    C2D_DrawRectSolid(0, HEADER_H, 0.5f, TOP_W, PATH_H, COL_PATHBAR);
    draw_text(cwd, 8, HEADER_H + 2, 0.4f, COL_TEXT_DIM);

    if (item_cnt == 0) {
        draw_text("(empty folder)", 30, LIST_TOP + 4, 0.45f, COL_TEXT_DIM);
    }

    for (int i = 0; i < ROWS_VISIBLE; ++i) {
        int idx = viewStart + i;
        if (idx >= item_cnt) break;
        float rowY = LIST_TOP + i * ROW_H;

        if (idx == cursor) {
            C2D_DrawRectSolid(0, rowY, 0.4f, LIST_W, ROW_H, COL_ROW_HILITE);
            C2D_DrawRectSolid(0, rowY, 0.45f, 4, ROW_H, COL_ACCENT);
        }

        float iconX = 8, iconY = rowY + 2;
        if (items[idx].isDir) draw_folder_icon(iconX, iconY);
        else                  draw_file_icon(iconX, iconY);

        char disp[NAME_LEN + 4];
        fit_name(items[idx].name, disp, sizeof(disp));
        if (items[idx].isDir && strcmp(items[idx].name, "..") != 0)
            strcat(disp, "/");

        draw_text(disp, 30, rowY + 2, 0.45f, COL_TEXT);
    }

    if (item_cnt > ROWS_VISIBLE) {
        int listH = ROWS_VISIBLE * ROW_H;
        C2D_DrawRectSolid(LIST_W, LIST_TOP, 0.4f, SCROLLBAR_W, listH, COL_SCROLL_TRACK);

        float thumbH = (float)listH * ROWS_VISIBLE / item_cnt;
        if (thumbH < 12) thumbH = 12;
        float range = item_cnt - ROWS_VISIBLE;
        float thumbY = LIST_TOP + (listH - thumbH) * (range > 0 ? (float)viewStart / range : 0);

        C2D_DrawRectSolid(LIST_W, thumbY, 0.45f, SCROLLBAR_W, thumbH, COL_SCROLL_THUMB);
    }
}

static void draw_bottom_screen(u32 ip, bool serverOk, bool connected) {
    C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, BOT_HEADER_H, COL_HEADER);
    draw_text("Status", 8, 5, 0.5f, COL_TEXT);

    draw_text("Device address", 12, 34, 0.4f, COL_TEXT_DIM);
    char ipLine[64];
    snprintf(ipLine, sizeof(ipLine), "%lu.%lu.%lu.%lu : %d",
             (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, PORT);
    draw_text(ipLine, 12, 50, 0.5f, COL_TEXT);

    u32 dotColor = !serverOk ? COL_ERR : connected ? COL_OK : COL_WAIT;
    C2D_DrawRectSolid(12, 78, 0.5f, 10, 10, dotColor);
    draw_text(connected ? "Client connected" : (serverOk ? "Waiting for connection" : "Server unavailable"),
               28, 76, 0.42f, COL_TEXT);

    draw_text("Last action", 12, 104, 0.4f, COL_TEXT_DIM);
    draw_text(g_statusMsg, 12, 120, 0.42f, g_statusColor);

    C2D_DrawRectSolid(12, 150, 0.5f, BOT_W - 24, 1, COL_PATHBAR);

    draw_control_hint(12, 164, "A", COL_OK, "Open / Send file");
    draw_control_hint(12, 188, "B", COL_WAIT, "Parent folder");
    draw_control_hint(170, 164, "Y", COL_ACCENT, "Refresh listing");
    draw_control_hint(170, 188, "START", COL_ERR, "Quit");
}

/* ---------------- main ---------------- */

int main(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C3D_RenderTarget *topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *botTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_dynBuf = C2D_TextBufNew(8192);

    u32 *socBuf = memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    int socRet = socBuf ? socInit(socBuf, SOC_BUFFERSIZE) : -1;
    bool socOk = (socRet == 0);

    int server = socOk ? start_server() : -1;
    u32 ip = socOk ? gethostid() : 0;

    if (!socOk)         set_status("Network init failed", COL_ERR);
    else if (server<0)  set_status("Server bind failed", COL_ERR);
    else                set_status("Waiting for connection...", COL_WAIT);

    refresh_dir();
    int pendingClient = -1;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kdown = hidKeysDown();
        if (kdown & KEY_START) break;

        if (server >= 0 && pendingClient < 0) {
            pendingClient = accept(server, NULL, NULL);
            if (pendingClient >= 0) {
                int fl = fcntl(pendingClient, F_GETFL, 0);
                fcntl(pendingClient, F_SETFL, fl & ~O_NONBLOCK);
                set_status("Client connected - pick a file, press A", COL_ACCENT);
            }
        }

        if (viewingImage) {
            if (kdown & KEY_B) {
                iv_close();
                viewingImage = false;
            } else if (kdown & KEY_A && pendingClient >= 0) {
                send_selected_file(pendingClient);
                close(pendingClient);
                pendingClient = -1;
                if (server >= 0) set_status("Waiting for connection...", COL_WAIT);
            } else if (kdown & (KEY_DOWN | KEY_UP)) {
                int dir = (kdown & KEY_DOWN) ? 1 : -1;
                int next = find_next_image(dir);
                if (next >= 0) {
                    cursor = next;
                    if (cursor < viewStart) viewStart = cursor;
                    if (cursor >= viewStart + ROWS_VISIBLE) viewStart = cursor - ROWS_VISIBLE + 1;

                    char path[512];
                    snprintf(path, sizeof(path), "%s%s", cwd, items[cursor].name);
                    if (!iv_open(path)) {
                        viewingImage = false;
                        set_status("Couldn't open image", COL_ERR);
                    }
                }
            }
        } else {
            if (kdown & KEY_UP) {
                if (cursor) --cursor;
                if (cursor < viewStart) viewStart = cursor;
            }
            if (kdown & KEY_DOWN) {
                if (cursor < item_cnt - 1) ++cursor;
                if (cursor >= viewStart + ROWS_VISIBLE) viewStart = cursor - ROWS_VISIBLE + 1;
            }
            if (kdown & KEY_B) {
                if (strcmp(cwd, "sdmc:/") != 0) { go_up(); refresh_dir(); }
            }
            if (kdown & KEY_Y) {
                refresh_dir();
                set_status("Refreshed", COL_TEXT_DIM);
            }
            if (kdown & KEY_A && item_cnt > 0) {
                if (items[cursor].isDir) {
                    if (strcmp(items[cursor].name, "..") == 0) {
                        go_up();
                    } else if (strlen(cwd) + strlen(items[cursor].name) + 2 < sizeof(cwd)) {
                        strcat(cwd, items[cursor].name);
                        strcat(cwd, "/");
                    }
                    refresh_dir();
                } else if (is_jpg(items[cursor].name)) {
                    char path[512];
                    snprintf(path, sizeof(path), "%s%s", cwd, items[cursor].name);
                    if (iv_open(path)) {
                        viewingImage = true;
                    } else {
                        set_status("Couldn't open image", COL_ERR);
                    }
                } else if (pendingClient >= 0) {
                    send_selected_file(pendingClient);
                    close(pendingClient);
                    pendingClient = -1;
                    if (server >= 0) set_status("Waiting for connection...", COL_WAIT);
                }
            }
        }

        C2D_TextBufClear(g_dynBuf);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        C2D_TargetClear(topTarget, COL_BG_TOP);
        C2D_SceneBegin(topTarget);
        draw_top_screen();

        C2D_TargetClear(botTarget, COL_BG_BOTTOM);
        C2D_SceneBegin(botTarget);
        if (viewingImage)
            iv_draw(g_dynBuf, items[cursor].name, pendingClient >= 0);
        else
            draw_bottom_screen(ip, server >= 0, pendingClient >= 0);

        C3D_FrameEnd(0);
    }

    if (pendingClient >= 0) close(pendingClient);
    if (server >= 0) close(server);
    if (socOk) socExit();
    free(socBuf);

    iv_close();
    C2D_TextBufDelete(g_dynBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}

/* ---------------------------------------------------------------------
 * SUMMARY OF CHANGES vs. the original console-based main.c
 * ---------------------------------------------------------------------
 * 1. Fixed the scrolling bug: the old print_list() indexed the visible
 *    row with `items[i]` instead of `items[viewStart + i]`, so once you
 *    scrolled, the cursor moved but the filenames stayed frozen on the
 *    first page.
 * 2. Replaced the text console with a citro2d scene graph: header bar,
 *    path bar, icon+highlight-bar file list, and a proportional
 *    scrollbar thumb that only appears when there's something to scroll.
 * 3. Replaced svcSleepThread(5ms) pacing with C3D_FrameBegin
 *    (C3D_FRAME_SYNCDRAW), which paces the loop to the screen's actual
 *    vsync instead of an arbitrary timer - this is what was causing the
 *    uneven/stuttery feel.
 * 4. Added a bottom-screen status panel: IP:port, a connection-state
 *    dot (grey = waiting, green = connected, red = server error), the
 *    last action/result message, and a button legend - all previously
 *    only reported via printf() lines that scrolled off-screen.
 * 5. Minor correctness fixes: the old ERROR reply sent length 4 for a
 *    6-byte string ("ERROR\n"); empty-folder cursor access is now
 *    guarded; socket/server fds are closed on the early-failure paths
 *    instead of leaking.
 * 6. No Makefile changes needed - it already linked citro2d/citro3d,
 *    they just weren't being used by the old console-only code.
 *
 * NOTE: this was written and reviewed against the standard citro2d API
 * (as used in the official devkitPro 3ds-examples), but I don't have a
 * devkitARM toolchain available to compile-test it in this sandbox.
 * Please build with `make` under devkitPro and try it in Citra or on
 * hardware before relying on it - if anything doesn't compile, the most
 * likely culprits are the C2D_DrawText/C2D_DrawTriangle argument order,
 * which are worth double-checking against your citro2d version first.
 * --------------------------------------------------------------------- */
