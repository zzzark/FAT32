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
    int cluster[FAT_COUNT];
};

struct FCB
{
    char filename[32];
    int size;
    int cluster;
    char time[16];  // yyyy-mm-dd-hh-mm-ss
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

void init_disk(void* buffer)
{
    struct DBR dbr;
    memset(&dbr, 0, DBR_SIZE);

    strcpy(dbr.flag, "zheng_rui_kun_2019151030");
    dbr.sector_size = SECTOR_SIZE;
    dbr.cluster_size = CLUSTER_SIZE;
    dbr.fat_count = FAT_COUNT;
    dbr.fat_size = FAT_SIZE;

    memcpy(buffer, &dbr, DBR_SIZE);
}

int main(void)
{
    // allocate buffer for disk ------------------------------
    int shm_id = 0;
    void* shm_buf = NULL;
    if (get_shm(&shm_buf, &shm_id) == -1) return 0;

    // initialize disk ------------------------------
    void* disk_buffer = shm_buf;
    init_disk(disk_buffer);

    // get disk data
    char cmd[256];

    while (1) {
        scanf("%s", cmd);
        if (0 == strcmp(cmd, "exit"))
            break;

        size_t offset = strtol(cmd, NULL, 0);
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





