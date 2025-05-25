#include <3ds.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>

#define PORT            8080
#define BUF_SIZE        1024
#define SOC_ALIGN       0x1000        // define locally (16 KB align)
#define SOC_BUFFERSIZE  0x100000      // 1 MB for sockets
#define MAX_ITEMS 	256
#define NAME_LEN	96
#define ROWS_VISIBLE	25

typedef struct { char name[NAME_LEN]; bool isDir; } DirEntry;

static DirEntry items[MAX_ITEMS];
static int item_cnt = 0;
static int cursor = 0;
static char cwd[256] = "sdmc:/";
const size_t ROOT_LEN = 6;
static int viewStart = 0;

static void refresh_dir(void) {
    item_cnt = 0;
    viewStart = 0;
    DIR *d = opendir(cwd);
    if (!d) return;

    if (strcmp(cwd, "sdmc:/") != 0) {
	strcpy(items[item_cnt].name, "..");
	items[item_cnt].isDir = true;
	++item_cnt;
    }

    struct dirent *e;
    while ((e = readdir(d)) && item_cnt < MAX_ITEMS) {
	if (e->d_name[0] == '.') continue;
	strncpy(items[item_cnt].name, e->d_name, NAME_LEN - 1);
	items[item_cnt].name[NAME_LEN - 1] = 0;
	items[item_cnt].isDir = (e->d_type == DT_DIR);
	++item_cnt;
    }
    closedir(d);
    cursor = 0;
}

static void print_list(u32 ip) {
    consoleClear();
    printf("3DS‑Sync Dir Browser (A=enter B=up Y=refresh START=quit)\n");
    printf("IP: %lu.%lu.%lu.%lu:%d\n", (ip>>24)&255,(ip>>16)&255,
           (ip>>8)&255, ip&255, PORT);
    printf("cwd: %s\n\n", cwd);

    for (int i = 0; i < ROWS_VISIBLE && (viewStart + i) < item_cnt; ++i) {
	int idx = viewStart + i;
        printf("%c %s%s\n",
		(idx == cursor ? '>' : ' '),
		items[i].name,
		items[i].isDir ? "/" : "");
    }
}

static int start_server(void)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        printf("socket() failed\n");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY; 

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("bind() failed\n");
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

int main(void) {

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    /* ---- socket init ---- */
    u32 *socBuf = memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!socBuf || socInit(socBuf, SOC_BUFFERSIZE)) {
        printf("socInit failed: 0x%08lX\n");
        svcSleepThread(3e9);
        return 1;
    }

    int server = start_server();
    if (server < 0) {
	printf("Server failed\n");
	return 1;
    }

    u32 ip = gethostid();
    refresh_dir();
    print_list(ip);
    int pendingClient = -1;
    bool ref = true;

    while (aptMainLoop()) {
	if (pendingClient < 0) { 
	    pendingClient = accept(server, NULL, NULL);
	    if (pendingClient >= 0) {
		int fl = fcntl(pendingClient, F_GETFL, 0);
		fcntl(pendingClient, F_SETFL, fl & ~O_NONBLOCK);

		printf("\nClient connected - highlight file and press A to send\n");
	    }
	}
	hidScanInput();
	u32 kdown = hidKeysDown();

	if (kdown & KEY_START) break;

	if (kdown & KEY_UP) { 
	    if (cursor) --cursor; 
	    ref = true;
	    if (cursor < viewStart) viewStart = cursor;
	}
	if (kdown & KEY_DOWN) { 
	    if (cursor < item_cnt-1) ++cursor;
	    ref = true;
	    if (cursor >= viewStart + ROWS_VISIBLE) viewStart = cursor - ROWS_VISIBLE + 1;
	}

	// Logic for Pressing Key A
	if (kdown & KEY_A) {
	    if (items[cursor].isDir) {
		if (strcmp(items[cursor].name, "..") == 0) {
		    size_t len = strlen(cwd);
		    if (len >0 && cwd[len - 1] == '/') cwd[len - 1] = '\0';
		    char *slash = strrchr(cwd, '/');
		    if (slash) slash[1] = '\0';
		} else {
		    if (strlen(cwd) + strlen(items[cursor].name) + 2 < sizeof(cwd)) {
			strcat(cwd, items[cursor].name);
			strcat(cwd, "/");
		    }
		}
		refresh_dir();
		ref = true;
	    } else if (pendingClient >= 0) {
		char path[512];
		snprintf(path, sizeof(path), "%s%s", cwd, items[cursor].name);

		FILE *fp = fopen(path, "rb");
		if (!fp) {
		    send(pendingClient, "ERROR\n", 4, 0);
		} else {
		    fseek(fp, 0, SEEK_END);
		    u32 size = ftell(fp);
		    fseek(fp, 0, SEEK_SET);

		    char hdr[128];
		    snprintf(hdr, sizeof(hdr), "FILE %s %u\n", items[cursor].name, size);
		    if (!send_all(pendingClient, hdr, strlen(hdr))) goto send_err;

		    char buf[BUF_SIZE];
		    size_t r;
		    while ((r = fread(buf, 1, sizeof(buf), fp)) > 0)
			if (!send_all(pendingClient, buf, r)) goto send_err;

		    printf("\nSent %s (%u bytes)\n", items[cursor].name, size);
		    goto send_done;

		    send_err:
		    printf("\n Transfer aborted (network error)\n");

		    send_done:
		    fclose(fp);
		}
		close(pendingClient);
		pendingClient = -1;
	    }
	}
	// Logic for going up directory
	if (kdown & KEY_B) {
	    if (strcmp(cwd, "sdmc:/") != 0) {
		size_t len = strlen(cwd);
		if (len >0 && cwd[len - 1] == '/') cwd[len - 1] = '\0';
		char *slash = strrchr(cwd, '/');
		if (slash) slash[1] = '\0';
		refresh_dir();
		ref = true;
	    }
	}
	if (kdown & KEY_Y) { refresh_dir(); ref = true; }
	
	if (ref) { print_list(ip); ref = false; }
	svcSleepThread(5 * 1000 * 1000LL);
    }
    
    if (pendingClient >= 0) close(pendingClient);
    close(server);
    socExit();
    free(socBuf);
    gfxExit();
    return 0;
}
