#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include <time.h>

#define MB ((size_t)(1<<20))
#define KB ((size_t)(1<<10))

#define DISK          ((size_t)100 * MB)            // 100MB
#define SECTOR_SIZE   ((size_t)512)                 // 512B
#define CLUSTER_SIZE  ((size_t)8 * SECTOR_SIZE)     // 4KB
#define FAT_COUNT     (DISK / CLUSTER_SIZE)         // 25600
#define FAT_SIZE      ((size_t)4 * FAT_COUNT)       // 100KB
#define DBR_SIZE      ((size_t)sizeof(struct DBR))

typedef unsigned int uint;

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

struct DirectoryEntity // SIZE == 64B
{
    char filename[32];
    char time[16];   // yyyymmdd hhmmss
    size_t size;     // file size, set to 0 for directory
    int type;        // not used(0), folder(1), file(2), volume(3)
    uint cluster;    // first cluster number of this file
};

char tmp_cluster[CLUSTER_SIZE];

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

void write_directory_item(void* memptr, char* filename, uint cluster, int type, int size)
{
    struct DirectoryEntity de;
    memset(&de, 0, sizeof(struct DirectoryEntity));

    strcpy(de.filename, filename);
    de.cluster = cluster;
    de.type = type;
    de.size = size;

    time_t now = time(NULL);
    struct tm *ptm = localtime(&now);
    sprintf(de.time, "%04d%02d%02d %02d%02d%02d", ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
    memcpy(memptr, &de, sizeof(struct DirectoryEntity));
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

    uint* fat1 = ((struct FAT*)(buffer + DBR_SIZE))->cluster;
    uint* fat2 = ((struct FAT*)(buffer + DBR_SIZE + FAT_SIZE))->cluster;

    fat1[0] = fat2[0] = 0xffffffff;
    fat1[1] = fat2[1] = 0xffffff0f;

    // root directory
    uint cluster = 2;
    fat1[cluster] = fat2[cluster] = 0xffffffff;

    void* memptr = cluster_to_pointer(buffer, cluster);
    write_directory_item(memptr, "ZRK", 0, 3, 0);  // volume

    memptr += sizeof(struct DirectoryEntity);
    write_directory_item(memptr, ".", cluster, 2, 0);  // current directory
}

uint get_next_cluster(void* buffer, uint curr_c)
{
    // get next cluster of current cluster (curr_c) along the list
    uint* fat1 = ((struct FAT*)(buffer + DBR_SIZE))->cluster;
    return fat1[curr_c];
}

uint get_last_cluster(void* buffer, uint curr_c)
{
    while (1) {
        uint cluster = get_next_cluster(buffer, curr_c);
        if (cluster == 0xffffffff) return curr_c;
        else curr_c = cluster;
    }
}

uint get_new_cluster(void* buffer, uint curr_c)
{
    // start from current cluster(curr_c), find the first available curr_c
    uint next_cl = curr_c;
    uint* fat1 = ((struct FAT*)(buffer + DBR_SIZE))->cluster;
    while (fat1[++next_cl] != 0)  { }
    fat1[curr_c] = next_cl;
    fat1[next_cl] = 0xffffffff;
    return next_cl;
}

void append_data_discrete(void* buffer, uint begin_c, void* data, size_t size)
{
    struct DirectoryEntity* de = cluster_to_pointer(buffer, begin_c);
    uint cluster = get_last_cluster(buffer, begin_c);
    void* memptr = cluster_to_pointer(buffer, cluster);  // last cluster ptr
    size_t allc, rest;

    if (de->type == 1) {  // folder
        allc = rest = 0;
        struct DirectoryEntity* sub = memptr;
        for (int i = 0; i < CLUSTER_SIZE / sizeof(struct DirectoryEntity); i++) {
            if (sub[i].type == 0) {
                allc = sizeof(struct DirectoryEntity) * (i + 1);
                rest = CLUSTER_SIZE - allc;
                break;
            }
        }
    }
    else if (de->type == 2) {  // file
        allc = (de->size % CLUSTER_SIZE);
        if (allc == 0) allc = de->size == 0 ? 0 : CLUSTER_SIZE;
        rest = CLUSTER_SIZE - allc;
    }
    else return;

    if (rest > size) {
        memcpy(memptr+allc, data, size);
    } else {
        size_t part1 = rest;
        size_t part2 = size - rest;
        uint new_c = get_new_cluster(buffer, cluster);
        void* ptr1 = memptr;
        void* ptr2 = cluster_to_pointer(buffer, new_c);

        memcpy(ptr1+allc, data, part1);
        memcpy(ptr2, data+allc, part2);
    }
}

void make_directory_at_ptr(void* buffer, size_t offset, uint cluster, uint p_cluster)
{
    void* memptr = cluster_to_pointer(buffer, cluster) + offset;
    write_directory_item(memptr, ".", cluster, 2, 0);
    memptr += sizeof(struct DirectoryEntity);
    write_directory_item(memptr, "..", p_cluster, 2, 0);
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

    char cmd[256];
    while (1) {
        scanf("%s", cmd);

        if (strcmp(cmd, "exit") == 0)
            break;
    }

    return 0;
}





