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

    int* fat1 = ((struct FAT*)(buffer + DBR_SIZE))->cluster;
    int* fat2 = ((struct FAT*)(buffer + DBR_SIZE + FAT_SIZE))->cluster;

    fat1[0] = fat2[0] = 0xffffffff;
    fat1[1] = fat2[1] = 0xffffff0f;
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


    return 0;
}





