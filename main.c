#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>

#define MB ((size_t)(1<<20))
#define KB ((size_t)(1<<10))

#define DISK          ((size_t)100 * MB)            // 100MB
#define SECTOR_SIZE   ((size_t)512)                 // 512B
#define CLUSTER_SIZE  ((size_t)8 * SECTOR_SIZE)     // 4KB
#define FAT_COUNT     (DISK / CLUSTER_SIZE)         // 25600
#define FAT_SIZE      ((size_t)4 * FAT_COUNT)       // 100KB
#define DBR_SIZE      ((size_t)sizeof(struct DBR))

struct DBR
{
    char flag[32];
    int sector_size;
    int cluster_size;
    int fat_count;
    int fat_size;
};

struct FAT
{
    uint cluster[FAT_COUNT];
};

struct FCB // SIZE == 64B
{
    char filename[32];
    char time[16];   // yyyymmdd hhmmss
    size_t size;     // file size, set to 0 for directory
    int type;        // not used(0), folder(1), file(2), volume(3)
    uint cluster;    // first cluster number of this file
};


int get_shm(void** pshm_buf, int* pshm_id)
{
    system("touch /tmp/zrk.shm");
    int shm_key = ftok("/tmp/zrk.shm", 0);
    if (shm_key == -1) {
        printf("Cannot get shm key.");
        return -1;
    }

    int shm_id = shmget(shm_key, DISK, 0666 | IPC_CREAT);
    void* shm_buf = shmat(shm_id, NULL, 0);

    printf("shmid is %d \n", shm_id);

    if (shm_buf == NULL) {
        printf("Cannot attach to shared memory buffer.");
        return -1;
    }

    *pshm_id = shm_id;
    *pshm_buf = shm_buf;
    return 0;
}

void rm_shm(void* shm_buf, int shm_id)
{
    shmdt(shm_buf);
    shmctl(shm_id, IPC_RMID, NULL);
}

void* cluster_to_pointer(void* buffer, int cluster)
{
    void* data = buffer + DBR_SIZE + FAT_SIZE + FAT_SIZE;
    return data + (cluster - 2) * CLUSTER_SIZE;
}


int main(void)
{
    // allocate buffer for disk ------------------------------
    int shm_id = 0;
    void* shm_buf = NULL;
    if (get_shm(&shm_buf, &shm_id) == -1) return 0;

    // initialize disk ------------------------------
    void* buffer = shm_buf;

    // get disk data
    char cmd[256];
    char param[256];

    printf(
            "usage: \n"
            "  exit\n"
            "  fat\n"
            "  fat+ [start]\n"
            "  cl [cluster]\n"
            "  [offset in bytes]\n"
            );

    size_t offset = 0;  // memory display offset
    while (1) {
        scanf("%s", cmd);
        if (0 == strcmp(cmd, "exit"))
            break;
        else if (0 == strcmp(cmd, "fat")) {
            uint* fat1 = ((struct FAT*)(buffer + DBR_SIZE))->cluster;
            for (int i = 0; i < 20; i++) {
                printf("%04x %08x\n", i, fat1[i]);
            }
            continue;
        }
        else if (0 == strcmp(cmd, "fat+")) {
            uint* fat1 = ((struct FAT*)(buffer + DBR_SIZE))->cluster;
            scanf("%s", param);
            uint start = strtol(param, NULL, 0);
            for (int i = start; i < start+20; i++) {
                printf("%04x %08x\n", i, fat1[i]);
            }
            continue;
        }
        else if (0 == strcmp(cmd, "cl")) {
            scanf("%s", param);
            uint cluster = strtol(param, NULL, 0);
            offset = cluster_to_pointer(buffer, cluster) - buffer;
        }
        else
            offset = strtol(cmd, NULL, 0);
        for (int i = 0; i < 20; i++) {
            printf("0x%016x  ", (unsigned int)(offset + i*16));
            for (int j = 0; j < 16; j++) {
                char* ptr = shm_buf + offset + i*16 + j;
                size_t lo = ((unsigned)(*ptr) & 0x0F) >> 0;
                size_t hi = ((unsigned)(*ptr) & 0xF0) >> 4;
                char* hex_table = "0123456789ABCDEF";
                printf("%c%c ", hex_table[hi], hex_table[lo]);
                if ((j+1) % 8 == 0) printf(" ");
            }
            printf("  |  ");
            for (int j = 0; j < 16; j++) {
                char* ptr = shm_buf + offset + i*16 + j;
                if (isprint(*ptr))
                    printf("%c", *ptr);
                else
                    printf("%c", '.');
                if ((j+1) % 8 == 0) printf(" ");
            }
            printf("\n");
        }
    }

    // release buffer
    rm_shm(shm_buf, shm_id);
    return 0;
}





