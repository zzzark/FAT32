#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>

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

struct FCB // SIZE == 64B
{
    char filename[32];
    char time[16];   // yyyymmdd hhmmss
    size_t size;     // file size, set to 0 for directory
    int type;        // not used(0), folder(1), file(2), volume(3)
    uint cluster;    // first cluster number of this file
};


int get_shm(void** pshm_buf, int* pshm_id, char* file, size_t size)
{
    char temp[256];
    sprintf(temp, "touch /tmp/%s", file);
    system(temp);
    sprintf(temp, "/tmp/%s", file);
    int shm_key = ftok(temp, 0);
    if (shm_key == -1) {
        printf("Cannot get shm key.");
        return -1;
    }

    int shm_id = shmget(shm_key, size, 0666 | IPC_CREAT);
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

struct Mutex
{
    char path[1024];
    int count;
};

#define MUTEX_COUNT 128
struct Mutex* mutex_list;

void init_mutex()
{
    void* shm_buf;
    int shm_id;
    get_shm(&shm_buf, &shm_id, "zrk_mutex.shm", sizeof(struct Mutex) * MUTEX_COUNT);
    mutex_list = shm_buf;
}

char* replace(char* mutex_path)
{
    char* ptr = mutex_path;
    while (*ptr != 0) {
        if (*ptr == '/') *ptr = '_';
        ptr++;
    }
    return mutex_path;
}

void release_mutex()
{
    struct Mutex* ptr = mutex_list;
    for (int i = 0; i < MUTEX_COUNT; i++, ptr++) {
        if (ptr->path[0] != 0) {
            char temp[1024];
            sprintf(temp, "rw_%s", ptr->path);  replace(temp);
            sem_unlink(temp);
            sprintf(temp, "r_%s",  ptr->path);  replace(temp);
            sem_unlink(temp);
            sprintf(temp, "w_%s",  ptr->path);  replace(temp);
            sem_unlink(temp);
            ptr->path[0] = 0;
        }
    }
}

struct Mutex* get_mutex(char* path)
{
    // search
    struct Mutex* ptr = mutex_list;
    for (int i = 0; i < MUTEX_COUNT; i++, ptr++) {
        if (strcmp(ptr->path, path) == 0) {
            return ptr;
        }
    }

    // alloc
    ptr = mutex_list;
    for (int i = 0; i < MUTEX_COUNT; i++, ptr++) {
        if (ptr->path[0] == 0) {
            struct Mutex* new_mutex = ptr;
            strcpy(new_mutex->path, path);
            new_mutex->count = 0;
            return new_mutex;
        }
    }

    return NULL;
}

void mutex_from_path(struct Mutex* mutex, sem_t** rw, sem_t** r, sem_t** w)
{
    char* path = mutex->path;
    char temp[1024];
    sprintf(temp, "rw_%s", path);  replace(temp);
    *rw = sem_open(temp, O_CREAT | O_RDWR, 0666, 1);
    sprintf(temp, "r_%s",  path);  replace(temp);
    *r  = sem_open(temp, O_CREAT | O_RDWR, 0666, 1);
    sprintf(temp, "w_%s",  path);  replace(temp);
    *w  = sem_open(temp, O_CREAT | O_RDWR, 0666, 1);
}

void wait_reader(struct Mutex* mutex)
{
    sem_t *rw, *r, *w;
    mutex_from_path(mutex, &rw, &r, &w);

    sem_wait(rw);
    sem_wait(r);
    if (mutex->count == 0) sem_wait(w);
    mutex->count++;
    sem_post(r);
    sem_post(rw);
}

void post_reader(struct Mutex* mutex)
{
    sem_t *rw, *r, *w;
    mutex_from_path(mutex, &rw, &r, &w);

    sem_wait(r);
    mutex->count--;
    if (mutex->count == 0) sem_post(w);
    sem_post(r);
}

void wait_writer(struct Mutex* mutex)
{
    sem_t *rw, *r, *w;
    mutex_from_path(mutex, &rw, &r, &w);

    sem_wait(rw);
    sem_wait(w);
}

void post_writer(struct Mutex* mutex)
{
    sem_t *rw, *r, *w;
    mutex_from_path(mutex, &rw, &r, &w);

    sem_post(w);
    sem_post(rw);
}

void* cluster_to_pointer(void* buffer, int cluster)
{
    void* data = buffer + DBR_SIZE + FAT_SIZE + FAT_SIZE;
    return data + (cluster - 2) * CLUSTER_SIZE;
}

void write_directory_item(void* memptr, char* filename, uint cluster, int type, int size)
{
    struct FCB de;
    memset(&de, 0, sizeof(struct FCB));

    strcpy(de.filename, filename);
    de.cluster = cluster;
    de.type = type;
    de.size = size;

    time_t now = time(NULL);
    struct tm *ptm = localtime(&now);
    sprintf(de.time, "%04d%02d%02d %02d%02d%02d", ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
    memcpy(memptr, &de, sizeof(struct FCB));
}

char* time_format(char*format, const char* time)
{
    format[0] = time[0];  // y
    format[1] = time[1];  // y
    format[2] = time[2];  // y
    format[3] = time[3];  // y
    format[4] = '-';
    format[5] = time[4];  // m
    format[6] = time[5];  // m
    format[7] = '-';
    format[8] = time[6];  // d
    format[9] = time[7];  // d
    format[10] = ' ';
    format[11] = time[9];  // h
    format[12] = time[10];  // h
    format[13] = ':';
    format[14] = time[11];  // m
    format[15] = time[12];  // m
    format[16] = ':';
    format[17] = time[13];  // s
    format[18] = time[14];  // s
    format[19] = 0;
    return format;
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

    memptr += sizeof(struct FCB);
    write_directory_item(memptr, ".", cluster, 1, 0);  // current directory
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
    // clear this cluster
    void* ptr = cluster_to_pointer(buffer, next_cl);
    memset(ptr, 0, CLUSTER_SIZE);
    return next_cl;
}

uint get_free_cluster(void* buffer, uint curr_c)
{
    uint next_cl = curr_c;
    uint* fat1 = ((struct FAT*)(buffer + DBR_SIZE))->cluster;
    uint* fat2 = ((struct FAT*)(buffer + DBR_SIZE + FAT_SIZE))->cluster;

    while (fat1[++next_cl] != 0)  { }
    // alloc new cluster
    fat1[next_cl] = fat2[next_cl] = 0xffffffff;
    return next_cl;
}

// TODO: tested for type == 2
// return: file size
size_t append_data_discrete(void* buffer, uint begin_c, void* data, size_t size)
{
    struct FCB* de = cluster_to_pointer(buffer, begin_c);
    uint cluster = get_last_cluster(buffer, begin_c);
    void* c_ptr = cluster_to_pointer(buffer, cluster);  // last cluster ptr
    size_t used, rest;

    if (de->type == 1) {  // folder
        used = rest = 0;  // already used size
        struct FCB* sub = c_ptr;
        for (int i = 0; i < CLUSTER_SIZE / sizeof(struct FCB); i++) {
            if (sub[i].type == 0) {
                used = sizeof(struct FCB) * (i + 1);
                rest = CLUSTER_SIZE - used;
                break;
            }
        }
    }
    else if (de->type == 2) {  // file
        used = (sizeof(struct FCB) + de->size) % CLUSTER_SIZE;
        if (used == 0) used = de->size == 0 ? 0 : CLUSTER_SIZE;
        rest = CLUSTER_SIZE - used;
    }
    else return de->size;

    if (rest > size) {
        memcpy(c_ptr + used, data, size);
    } else {
        size_t part1 = rest;
        size_t part2 = size - rest;
        uint new_c = get_new_cluster(buffer, cluster);
        void* ptr1 = c_ptr;
        void* ptr2 = cluster_to_pointer(buffer, new_c);

        memcpy(ptr1 + used, data, part1);
        memcpy(ptr2, data + used, part2);
    }
    de->size += size;
    return de->size;
}

void init_directory_at_cluster(void* buffer, uint cluster, uint p_cluster)
{
    void* memptr = cluster_to_pointer(buffer, cluster);
    write_directory_item(memptr, ".", cluster, 1, 0);
    memptr += sizeof(struct FCB);
    write_directory_item(memptr, "..", p_cluster, 1, 0);
}

// file exists:  cluster of that file / folder
// not exists:   0
uint find_file_at_cluster(void* buffer, uint cluster, char* filename)
{
    while (cluster != 0xffffffff) {
        void* c_ptr = cluster_to_pointer(buffer, cluster);

        size_t offset = 0;
        while (offset < CLUSTER_SIZE) {
            struct FCB* fcb = (struct FCB *)(c_ptr + offset);
            if (fcb->type == 1 || fcb->type == 2)  // is a file or directory
            {
                char* name = fcb->filename;
                if (strcmp(name, filename) == 0) {
                    return fcb->cluster;
                }
            }
            offset += sizeof(struct FCB);
        }
        cluster = get_next_cluster(buffer, cluster);
    }
    return 0;
}

// not found:  0
// found:      cluster of that file / folder
uint path_to_cluster(void* buffer, char* path)
{
    if (strlen(path) == 1 && path[0] == '/')
        return 2;  // root
    if (path[0] != '/')
        return 0;
    char cache[1024];
    strcpy(cache, path);
    uint cluster = 2;  // start from root "/"
    char* filename = strtok(cache, "/");
    while (filename != NULL) {
        cluster = find_file_at_cluster(buffer, cluster, filename);
        if (cluster == 0)
            return 0;
        filename = strtok(NULL, "/");
    }
    return cluster;
}

// success: newly allocated cluster
// fail:    0
uint append_fcb_at_cluster(void* buffer, uint cluster, char* filename, int type, int size)
{
    if (find_file_at_cluster(buffer, cluster, filename) != 0)
        return 0;

got_new_block:
    while (1) {
        void* c_ptr = cluster_to_pointer(buffer, cluster);

        size_t offset = 0;
        while (offset < CLUSTER_SIZE) {
            struct FCB* fcb = (struct FCB *)(c_ptr + offset);
            if (fcb->type == 0) { // not a file <=> is a free fcb
                uint free_cluster = get_free_cluster(buffer, cluster);
                write_directory_item((c_ptr + offset), filename, free_cluster, type, size);
                return free_cluster;
            }
            offset += sizeof(struct FCB);
        }
        uint next = get_next_cluster(buffer, cluster);
        if (next == 0xffffffff) break;
        cluster = next;
    }
    cluster = get_new_cluster(buffer, cluster);
    goto got_new_block;
}

void reset_fat_along_cluster_chain(void* buffer, uint cluster)
{
    uint* fat1 = ((struct FAT*)(buffer + DBR_SIZE))->cluster;
    while (cluster != 0xffffffff) {
        uint next = fat1[cluster];
        fat1[cluster] = 0;
        cluster = next;
    }
}

struct FCB* get_fcb_of_file_at_cluster(void* buffer, uint cluster, char* filename)
{
    void* c_ptr = cluster_to_pointer(buffer, cluster);
    size_t offset = 0;
    while (offset < CLUSTER_SIZE) {
        struct FCB* fcb = (struct FCB *)(c_ptr + offset);
        if (fcb->type != 3 && fcb->type != 0 && strcmp(fcb->filename, filename) == 0) {
            return fcb;
        }
        offset += sizeof(struct FCB);
    }
    return NULL;
}

// success:         1
// not found:       0
// type not match: -1 (remove file or directory)
int remove_fcb_at_cluster(void* buffer, uint p_cluster, char* filename, int type)
{
    void* c_ptr = cluster_to_pointer(buffer, p_cluster);
    size_t offset = 0;
    while (offset < CLUSTER_SIZE) {
        struct FCB* fcb = (struct FCB *)(c_ptr + offset);
        if (fcb->type != 3 && fcb->type != 0 && strcmp(fcb->filename, filename) == 0) {
            if (fcb->type != type)
                return -1;
            if (fcb->type == 1) {  // remove directory
                void* sub_ptr = cluster_to_pointer(buffer, fcb->cluster);
                size_t sub_off = 2 * sizeof(struct FCB);  // ignore "." and ".."
                while (sub_off < CLUSTER_SIZE) {
                    struct FCB* sub_fcb = (struct FCB *)(sub_ptr + sub_off);
                    remove_fcb_at_cluster(buffer, fcb->cluster, sub_fcb->filename, sub_fcb->type);
                    sub_off += sizeof(struct FCB);
                }
            }
            reset_fat_along_cluster_chain(buffer, fcb->cluster);
            memset(fcb, 0, sizeof(struct FCB));
            return 1;
        }
        offset += sizeof(struct FCB);
    }
    return 0;
}

// =========================================== COMMAND IMPLEMENTATION =========================================== //

// invalid:  0
// valid:    1
int split_path(char* path, char* p_path, char* c_file)
{
    for (size_t i = strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') {
            strcpy(p_path, path);
            p_path[i] = 0;
            strcpy(c_file, &path[i + 1]);
            break;
        }
        if (i == 0) return 0;
    }
    if (strlen(p_path) == 0) {
        p_path[0] = '/';
        p_path[1] = 0;
    }
    return 1;
}

// success:       1
// fail:          0
// file exists:  -1
int mkdir_(void* buffer, char* path)
{
    char p_path[1024] = {0};  // parent path
    char c_file[1024] = {0};  // child file
    if (split_path(path, p_path, c_file) == 0)
        return 0;
    uint cluster = path_to_cluster(buffer, p_path);
    if (cluster == 0) return 0;

    uint c_cl = append_fcb_at_cluster(buffer, cluster, c_file, 1, 0);
    if (c_cl == 0)
        return -1;
    init_directory_at_cluster(buffer, c_cl, cluster);
    return 1;
}

// success:          1
// fail:             0
// file not found:  -1
// not match:        2
int rmdir_(void* buffer, char* path)
{
    char p_path[1024] = {0};  // parent path
    char c_file[1024] = {0};  // child file
    if (split_path(path, p_path, c_file) == 0)
        return 0;
    uint cluster = path_to_cluster(buffer, p_path);
    if (cluster == 0)
        return -1;
    int r = remove_fcb_at_cluster(buffer, cluster, c_file, 1);
    if (r == 0) return -1;
    if (r == -1) return 2;
    return 1;
}

// success:          1
// fail:             0
// file not found:  -1
// not match:        2
int rm_(void* buffer, char* path)
{
    char p_path[1024] = {0};  // parent path
    char c_file[1024] = {0};  // child file
    if (split_path(path, p_path, c_file) == 0)
        return 0;
    uint cluster = path_to_cluster(buffer, p_path);
    if (cluster == 0)
        return -1;
    int r = remove_fcb_at_cluster(buffer, cluster, c_file, 2);
    if (r == 0) return -1;
    if (r == -1) return 2;
    return 1;
}

// success:          1
// file exists:      0
// file not found:  -1
int open_(void* buffer, char* path)
{
    char p_path[1024] = {0};  // parent path
    char c_file[1024] = {0};  // child file
    if (split_path(path, p_path, c_file) == 0)
        return -1;
    uint cluster = path_to_cluster(buffer, p_path);
    if (cluster == 0)
        return -1;

    uint c_cl = append_fcb_at_cluster(buffer, cluster, c_file, 2, 0);
    if (c_cl == 0)
        return 0;
    void* memptr = cluster_to_pointer(buffer, c_cl);
    write_directory_item(memptr, c_file, c_cl, 2, 0);
    printf("append data:");
    fflush(stdout);
    char data[1024];
    scanf("%s", data);
    printf("writing data: [%s]\n", data);
    size_t f_size = append_data_discrete(buffer, c_cl, data, strlen(data));
    struct FCB* fcb = get_fcb_of_file_at_cluster(buffer, cluster, c_file);
    fcb->size = f_size;
    return 1;
}

// success:          1
// file not found:   0
// path not found:  -1
int append_(void* buffer, char* path, char ch, size_t size)
{
    char p_path[1024] = {0};  // parent path
    char c_file[1024] = {0};  // child file
    if (split_path(path, p_path, c_file) == 0)
        return 0;
    uint cluster = path_to_cluster(buffer, p_path);
    if (cluster == 0)
        return -1;

    uint c_cl = find_file_at_cluster(buffer, cluster, c_file);
    char* data = malloc(size);
    memset(data, ch, size);
    size_t f_size = append_data_discrete(buffer, c_cl, data, size);
    struct FCB* fcb = get_fcb_of_file_at_cluster(buffer, cluster, c_file);
    fcb->size = f_size;
    free(data);
    return 1;
}


// success:         1
// not a directory: 0
int ls_(void* buffer, char* path)
{
    uint cluster = path_to_cluster(buffer, path);
    if (cluster == 0) return 0;
    struct FCB* this_fcb = (struct FCB*)cluster_to_pointer(buffer, cluster);
    if (this_fcb->type != 3 && this_fcb->type != 1)
        return 0;
    void* c_ptr = cluster_to_pointer(buffer, cluster);
    size_t offset = 0;
    printf("%-13s\t%8s\t%s\n", "name", "size", "created time");
    printf("-------------\t--------\t-------------------\n");
    while (offset < CLUSTER_SIZE) {
        struct FCB* fcb = (struct FCB *)(c_ptr + offset);
        char t_fmt[256];
        if (fcb->type == 1) {
            printf("[D]%-10s\t%8zu\t%s\n", fcb->filename, fcb->size, time_format(t_fmt, fcb->time));
        } else if (fcb->type == 2) {
            printf("[F]%-10s\t%8zu\t%s\n", fcb->filename, fcb->size, time_format(t_fmt, fcb->time));
        }
        offset += sizeof(struct FCB);
    }
    return 1;
}

// success:        1
// not exists:    -1
int rename_(void* buffer, char* path, char* new_name)
{
    char p_path[1024] = {0};  // parent path
    char c_file[1024] = {0};  // child file
    if (split_path(path, p_path, c_file) == 0)
        return -1;
    uint p_cl = path_to_cluster(buffer, p_path);
    struct FCB* fcb = get_fcb_of_file_at_cluster(buffer, p_cl, c_file);
    if (fcb == NULL) return -1;
    uint c_cl = fcb->cluster;
    strcpy(fcb->filename, new_name);
    if (fcb->type == 2) {
        struct FCB* c_fcb = cluster_to_pointer(buffer, c_cl);
        strcpy(c_fcb->filename, new_name);
    }
    return 1;
}


int main(void)
{
    init_mutex();
    struct Mutex* mutex = get_mutex("/aa");
    
    printf("reader: getting mutex\n");
    wait_reader(mutex);

    printf("reader: reading\n");
    char temp[222];
    scanf("%s", temp);

    post_reader(mutex);
    printf("reader: done\n");

    release_mutex();
    return 0;
    // allocate buffer for disk ------------------------------
    int shm_id = 0;
    void* shm_buf = NULL;
    if (get_shm(&shm_buf, &shm_id, "zrk.shm", DISK) == -1) return 0;

    // initialize disk ------------------------------
    void* disk_buffer = shm_buf;
    init_disk(disk_buffer);

    char cmd[256];
    char param1[256];
    char param2[256];
    while (1) {
        printf(">>");
        fflush(stdout);
        scanf("%s", cmd);
        if (strcmp(cmd, "mkdir") == 0) {
            scanf("%s", param1);
            printf("making directory [%s] ...\n", param1);
            int ret = mkdir_(disk_buffer, param1);
            if (ret == 0)
                printf("error!\n");
            if (ret == -1)
                printf("file already exists!\n");
        }
        else if (strcmp(cmd, "rmdir") == 0) {
            scanf("%s", param1);
            printf("removing directory [%s] ...\n", param1);
            int ret = rmdir_(disk_buffer, param1);
            if (ret == 0)
                printf("error!\n");
            if (ret == -1)
                printf("file not found!\n");
            if (ret == 2)
                printf("this is a file\n");
        }
        else if (strcmp(cmd, "ls") == 0) {
            scanf("%s", param1);
            int ret = ls_(disk_buffer, param1);
            if (ret == 0)
                printf("not a directory!\n");
        }
        else if (strcmp(cmd, "open") == 0) {
            scanf("%s", param1);
            int ret = open_(disk_buffer, param1);
            if (ret == 0)
                printf("file already exists!\n");
            if (ret == -1)
                printf("path not found!\n");
        }
        else if (strcmp(cmd, "append") == 0) {
            scanf("%s", param1);
            char ch[256];  uint size;
            printf("char:\n");
            scanf("%s", ch);
            printf("size:\n");
            scanf("%u", &size);
            int ret = append_(disk_buffer, param1, ch[0], size);
            if (ret == -1)
                printf("path not found!\n");
            if (ret == 0)
                printf("file not found!\n");
        }
        else if (strcmp(cmd, "rm") == 0) {
            scanf("%s", param1);
            int ret = rm_(disk_buffer, param1);
            if (ret == 0)
                printf("error!\n");
            if (ret == -1)
                printf("file not found!\n");
            if (ret == 2)
                printf("not a file!\n");
        }
        else if (strcmp(cmd, "rename") == 0) {
            scanf("%s", param1);
            scanf("%s", param2);
            int ret = rename_(disk_buffer, param1, param2);
            if (ret == -1)
                printf("file not found!\n");
        }
        else if (strcmp(cmd, "exit") == 0) {
            break;
        }
        else
            printf("command not found: %s\n", cmd);
    }
    return 0;
}





