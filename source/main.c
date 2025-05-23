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
    printf("Press START to quit.\n\n");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;

        int client = accept(server, NULL, NULL);
        if (client < 0) {
            /* EAGAIN / EWOULDBLOCK just means no one connected yet */
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            printf("accept() err %d\n", errno);
            break;
        }

        DIR *d = opendir("sdmc:/3ds_sync");
        if (!d) {
            const char *msg = "ERROR: sdmc:/3ds_sync not found\r\n";
            send(client, msg, strlen(msg), 0);
        } else {
            struct dirent *ent;
            char line[BUF_SIZE];
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_type == DT_REG) {
                    snprintf(line, sizeof(line), "%s\r\n", ent->d_name);
                    send(client, line, strlen(line), 0);
                }
            }
            closedir(d);
        }
        close(client);
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
    start_server();

    socExit();
    free(socBuf);
    gfxExit();
    return 0;
}

