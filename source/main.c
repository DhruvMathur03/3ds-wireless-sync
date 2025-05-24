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
#define MAX_FILES 	256
#define NAME_LEN	64

static char files[MAX_FILES][NAME_LEN];
static int file_cnt = 0;

static void scan_and_print(void)
{
    file_cnt = 0;
    consoleClear();
    printf("3DS-Sync	(START=quit, Y=refresh)\n\n");

    DIR *d = opendir("sdmc:/3ds_sync");
    if (!d) {
	printf("[sdmc:/3ds_sync not found]\n");
	return;
    }
    
    struct dirent *ent;
    while ((ent = readdir(d)) && file_cnt < MAX_FILES) {
	if (ent->d_type == DT_REG) {
	    strncpy(files[file_cnt], ent->d_name, NAME_LEN - 1);
	    files[file_cnt][NAME_LEN - 1] = 0;
	    printf("%s\n", files[file_cnt]);
	    ++file_cnt;
	}
    }
    closedir(d);

    if (file_cnt == 0) printf("[folder empty]\n");
}

static void start_server(void)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        printf("socket() failed\n");
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    // addr.sin_addr.s_addr = gethostid();
    addr.sin_addr.s_addr = INADDR_ANY;   // listen on all interfaces

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("bind() failed\n");
        close(server);
        return;
    }

    listen(server, 4);

    /* make listening socket non‑blocking */
    int flags = fcntl(server, F_GETFL, 0);
    fcntl(server, F_SETFL, flags | O_NONBLOCK);

    printf("Listening on %lu.%lu.%lu.%lu:%d\n",
           (addr.sin_addr.s_addr >> 24) & 0xFF,
           (addr.sin_addr.s_addr >> 16) & 0xFF,
           (addr.sin_addr.s_addr >>  8) & 0xFF,
           (addr.sin_addr.s_addr      ) & 0xFF,
           PORT);

    while (aptMainLoop()) {
        hidScanInput();
	u32 kd = hidKeysDown();
        if (kd & KEY_START) break;
	if (kd & KEY_Y) scan_and_print();    

        int client = accept(server, NULL, NULL);
        if (client >= 0) { 
	    for (int i = 0; i < file_cnt; ++i) {
		send(client, files[i], strlen(files[i]), 0);
		send(client, "\r\n", 2, 0);
	    }
	    close(client);
	} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            printf("accept() err %d\n", errno);
	    svcSleepThread(5 * 1000 * 1000 * 1000LL);
        }
    }
    close(server);
}

int main(void)
{

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    /* ---- socket init ---- */
    u32 *socBuf = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!socBuf) {
        printf("memalign failed\n");
        gfxExit();
        return 1;
    }
    Result rc = socInit(socBuf, SOC_BUFFERSIZE);
    if (R_FAILED(rc)) {
        printf("socInit failed: 0x%08lX\n", rc);
        free(socBuf);
	gfxExit();
        return 1;
    }

    printf("Starting file‑listing server …\n");
    scan_and_print();
    start_server();

    socExit();
    free(socBuf);
    gfxExit();
    return 0;
}
