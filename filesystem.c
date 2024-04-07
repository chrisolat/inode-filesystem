#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "filesystem.h"
#include "softwaredisk.h"

#define MAX_FILES                   850
#define DATA_BITMAP_BLOCK           0   // max SOFTWARE_DISK_BLOCK_SIZE*8 bits
#define INODE_BITMAP_BLOCK          1   // max SOFTWARE_DISK_BLOCK_SIZE*8 bits
#define FIRST_INODE_BLOCK           2
#define LAST_INODE_BLOCK            55  // 16 inodes per block, 
                                            // max of 54 *16 = 846 inodes

#define FIRST_DIR_ENTRY_BLOCK       56
#define LAST_DIR_ENTRY_BLOCK        905     // max files = 905-56+1 = 8550
#define FIRST_DATA_BLOCK            906
#define LAST_DATA_BLOCK             5000
#define MAX_FILENAME_SIZE           507
#define NUM_DIRECT_INODE_BLOCKS     13
#define NUM_SINGLE_INDIRECT_BLOCKS  (SOFTWARE_DISK_BLOCK_SIZE / sizeof(uint16_t))

#define MAX_FILE_SIZE               (NUM_DIRECT_INODE_BLOCKS + NUM_SINGLE_INDIRECT_BLOCKS) * SOFTWARE_DISK_BLOCK_SIZE


// Type for one inode. Structure must be 32 bytes long.
typedef struct Inode {
    uint32_t size;                          // file size in bytes.
    uint16_t b[NUM_DIRECT_INODE_BLOCKS+1];  // direct blocks + 1 indirect block

} Inode;
// !!!!!!!!! needs comments?

typedef struct IndirectBlock {
    uint16_t b[NUM_SINGLE_INDIRECT_BLOCKS];
}   IndirectBlock;

typedef struct InodeBlock{
    Inode inodes[SOFTWARE_DISK_BLOCK_SIZE/sizeof(Inode)];
} InodeBlock;

typedef struct DirEntry{
    uint8_t file_is_open;
    uint16_t inode_idx;
    char name[MAX_FILENAME_SIZE];
}   DirEntry;


typedef struct FreeBitmap{
    uint8_t bytes[SOFTWARE_DISK_BLOCK_SIZE];
} FreeBitmap;

typedef struct FileInternals{
    uint32_t  position;
    FileMode mode;
    Inode inode;
    DirEntry d;
    uint16_t d_block;
} FileInternals;




int32_t find_zero_bit(uint8_t *data){
    int32_t i, j;
    int32_t freeidx = -1;
    
    for(i = 0; i < SOFTWARE_DISK_BLOCK_SIZE && freeidx < 0; i++){
        for(j = 0; j <= 7 && freeidx < 0; j++){
            if((data[i] & (1 << (7 - j))) == 0){
                freeidx = i*8+j;
            }
        }
    }
    return freeidx;
}



uint16_t find_free_inode_idx(void){
    FreeBitmap f;
    if(read_sd_block(&f, INODE_BITMAP_BLOCK)){
        return find_zero_bit(f.bytes);
    }
    else{
        fserror = FS_IO_ERROR;
        return -1;
    }
}


int32_t find_free_block_idx(void){
    FreeBitmap f;
    if(read_sd_block(&f, DATA_BITMAP_BLOCK)){
        return find_zero_bit(f.bytes)+FIRST_DATA_BLOCK;
    }
    else{
        fserror=FS_IO_ERROR;
        return -1;
    }
}


uint32_t mark_block(uint16_t blk, uint32_t flag){
    FreeBitmap f;
    blk -= FIRST_DATA_BLOCK;
    if(!read_sd_block(&f, DATA_BITMAP_BLOCK)){
        return 0;
    }
    else{
        if(flag){
            f.bytes[blk / 8] |= (1 << (7 - (blk % 8)));
        }
        else{
            f.bytes[blk / 8] &= ~(1 << (7 - (blk % 8)));
        }
        if(!write_sd_block(&f, DATA_BITMAP_BLOCK)){
            return 0;
        }
    }
    return 1;
}


uint32_t mark_inode(uint16_t i, uint32_t flag){
    FreeBitmap f;
    if(! read_sd_block(&f, INODE_BITMAP_BLOCK)){
        fserror = FS_IO_ERROR;
        return 0;
    }
    else{
        if(flag){
            f.bytes[i / 8] |= (1 << (7 - (i % 8)));
        }
        else{
            f.bytes[i / 8] &= ~(1 << (7 - (i % 8)));
        }
        if(! write_sd_block(&f, INODE_BITMAP_BLOCK)){
            fserror = FS_IO_ERROR;
            return 0;
        }
    }
    return 1;
}


uint32_t read_inode(uint16_t idx, struct Inode* inode){
    InodeBlock ib;

    uint16_t inodeblock = idx / sizeof(uint16_t) + FIRST_INODE_BLOCK;
    bzero(inode, sizeof(Inode));

    if(inodeblock > LAST_INODE_BLOCK){
        return 0;
    }
    else{
        if(read_sd_block(&ib, inodeblock)){
            *inode=ib.inodes[idx % sizeof(uint16_t)];
            return 1;
        }
        else{
            fserror = FS_IO_ERROR;
            return 0;
        }
    }
}


uint32_t free_inode(uint16_t i){
    Inode inode;
    IndirectBlock id;
    int j;

    if(!read_inode(i, &inode)){
        fserror = FS_IO_ERROR;
        return 0;
    }
    else{
        for(j = 0; j < NUM_DIRECT_INODE_BLOCKS; j++){
            if(inode.b[j]){
                if(!mark_block(inode.b[j], 0)){
                    fserror=FS_IO_ERROR;
                    return 0;
                }
            }
        }

        if(inode.b[NUM_DIRECT_INODE_BLOCKS]){
            if(!read_sd_block(&id, inode.b[NUM_DIRECT_INODE_BLOCKS])){
                fserror=FS_IO_ERROR;
                return 0;
            }
            else{
                for(j = 0; j < NUM_SINGLE_INDIRECT_BLOCKS; j++){
                    if(id.b[j]){
                        if(! mark_block(id.b[j],0)){
                            fserror = FS_IO_ERROR;
                            return 0;
                        }
                    }
                }
            }
        }
        if(! mark_inode(i,0)){
            return 0;
        }
    }
    return 1;
}


uint32_t write_inode(uint16_t idx, struct Inode inode){
    InodeBlock ib;
    
    uint16_t inodeblock = idx / sizeof(uint16_t) + FIRST_INODE_BLOCK;
    if(inodeblock <= LAST_INODE_BLOCK){
        if(!read_sd_block(&ib, inodeblock)){
            fserror = FS_IO_ERROR;
            return 0;
        }
        else{
            ib.inodes[idx % sizeof(uint16_t)] = inode;
            if(!write_sd_block(&ib, inodeblock)){
                fserror = FS_IO_ERROR;
                return 0;
            }
        }
    }
    return 1;
}

int32_t get_inode_block_number(Inode inode, uint16_t blocknum){
    IndirectBlock id;

    if(blocknum < NUM_DIRECT_INODE_BLOCKS){
        return inode.b[blocknum];
    }
    else if(! inode.b[NUM_DIRECT_INODE_BLOCKS]){
        return 0;
    }
    else if(blocknum - NUM_DIRECT_INODE_BLOCKS <= NUM_SINGLE_INDIRECT_BLOCKS){
        if(! read_sd_block(&id, inode.b[NUM_DIRECT_INODE_BLOCKS])){
            return -1;
        }
        else{
            return id.b[blocknum - NUM_DIRECT_INODE_BLOCKS];
        }
    }
    else{
        return -1;
    }
}

int32_t set_inode_block_number(File f, uint16_t blocknum, uint16_t newblock){
    IndirectBlock id;
    int32_t new_id_block;

    if(blocknum < NUM_DIRECT_INODE_BLOCKS){
        f->inode.b[blocknum] = newblock;
    }
    else if (blocknum - NUM_DIRECT_INODE_BLOCKS > NUM_SINGLE_INDIRECT_BLOCKS){
        return 0;
    }
    else{
        if(! f->inode.b[NUM_DIRECT_INODE_BLOCKS]){
            bzero(&id, sizeof(IndirectBlock));
            new_id_block = find_free_block_idx();
            if(new_id_block < 0){
                return 0;
            }
            if(! mark_block(new_id_block, 1)){
                fserror = FS_IO_ERROR;
                return 0;
            }
            f->inode.b[NUM_DIRECT_INODE_BLOCKS] = new_id_block;
        }
        else{
            if(! read_sd_block(&id, f->inode.b[NUM_DIRECT_INODE_BLOCKS])){
                fserror = FS_IO_ERROR;
                return 0;
            }
        }

        id.b[blocknum - NUM_DIRECT_INODE_BLOCKS] = newblock;

        if(!write_sd_block(&id, f->inode.b[NUM_DIRECT_INODE_BLOCKS])){
            fserror = FS_IO_ERROR;
            return 0;
        }
    }
    if(! mark_block(newblock, 1)){
        fserror = FS_IO_ERROR;
        return 0;
    }
    if(!write_inode(f->d.inode_idx, f->inode)){
        fserror = FS_IO_ERROR;
        return 0;
    }
    return 1;
}

uint32_t read_data_block(File f, uint16_t blocknum, void* data){
    uint16_t datablock;
    
    datablock = get_inode_block_number(f->inode, blocknum);
    if(datablock >= FIRST_DATA_BLOCK && datablock <= LAST_DATA_BLOCK){
        if(!read_sd_block(data, datablock)){
            fserror=FS_IO_ERROR;
            return 0;
        }
    }
    else if (datablock == 0){
        bzero(data, SOFTWARE_DISK_BLOCK_SIZE);
    }
    else{
        return 0;
    }
    return 1;
}

uint32_t write_data_block(File f, uint16_t blocknum, void* data){
    uint16_t datablock;

    datablock = get_inode_block_number(f->inode, blocknum);
    if (datablock == 0){
        datablock = find_free_block_idx();
        if(datablock < 0){
            fserror=FS_OUT_OF_SPACE;
            return 0;
        }
        else{
            if(!mark_block(datablock, 1)){
                fserror = FS_IO_ERROR;
                return 0;
            }
            if(!set_inode_block_number(f,blocknum,datablock)){
                return 0;
            }
        }
    }

    if(datablock >= FIRST_DATA_BLOCK && datablock <= LAST_DATA_BLOCK){
        if(!write_sd_block(data,datablock)){
            fserror = FS_IO_ERROR;
            return 0;
        }
    }
    else{
        return 0;
    }
    return 1;
}

int32_t find_directory_entry(char* filename, DirEntry* d){
    int32_t block = -1;
    int i;

    for(i = FIRST_DIR_ENTRY_BLOCK; i <= LAST_DIR_ENTRY_BLOCK && block < 0; i++){
        if(!read_sd_block(d,i)){
            fserror=FS_IO_ERROR;
            break;
        }
        if(!strncmp(filename, d->name, MAX_FILENAME_SIZE-1)){
            block = i;
        }
    }
    if(block < 0){
        bzero(d,sizeof(DirEntry));
    }
    return block;
}

uint32_t write_directory_entry(DirEntry d, uint16_t block){
    if(!write_sd_block(&d, block)){
        fserror=FS_IO_ERROR;
        return 0;
    }
    else{
        return -1;
    }
}

int32_t create_directory_entry(DirEntry d){
    int32_t block = -1;
    int i;
    DirEntry temp;

    for (i=FIRST_DIR_ENTRY_BLOCK; i <= LAST_DIR_ENTRY_BLOCK && block < 0; i++){
        if(!read_sd_block(&temp, i)){
            fserror=FS_IO_ERROR;
            break;
        }
        // set index to this block if available
        if(!temp.name[0]){
            block=i;
        }
    }

    if(block != -1 && !write_directory_entry(d, block)){
        block=-1;
    }
    return block;
}

uint32_t delete_directory_entry(DirEntry d, uint16_t block){

    free_inode(d.inode_idx);
    bzero(&d, sizeof(DirEntry));
    if(!write_sd_block(&d, block)){
        fserror=FS_IO_ERROR;
        return 0;
    }
    else{
        return 1;
    }
}


// public functions


File open_file(char* name, FileMode mode){
    int32_t d_block;
    File f=malloc(sizeof(FileInternals));

    fserror=FS_NONE;
    bzero(f, sizeof(FileInternals));
    f->mode = mode;
    f->position = 0;
    fserror=FS_NONE;

    d_block = find_directory_entry(name, &(f->d));
    if(d_block < 0){
        fserror = FS_FILE_NOT_FOUND;
        goto err;
    }

    f->d_block = d_block;
    if(f->d.file_is_open){
        fserror = FS_FILE_OPEN;
        goto err;
    }
    f->d.file_is_open=1;
    if(!write_directory_entry(f->d, f->d_block)){
        fserror=FS_IO_ERROR;
        goto err;
    }

    if(!read_inode(f->d.inode_idx, &(f->inode))){
        fserror=FS_IO_ERROR;
        goto err;
    }
done:
    return f;
err:
    free(f);
    return 0;
}


File create_file(char* name){
    int32_t d_block;
    File f = malloc(sizeof(FileInternals));

    fserror = FS_NONE;
    bzero(f, sizeof(FileInternals));

    if(!name[0]){
        fserror=FS_ILLEGAL_FILENAME;
        goto err;
    }

    d_block = find_directory_entry(name, &(f->d));
    if(d_block >= 0){
        fserror = FS_FILE_ALREADY_EXISTS;
        goto err;
    }

    f->mode = READ_WRITE;
    f->position = 0;
    fserror = FS_NONE;
    if((f->d.inode_idx = find_free_block_idx()) < 0){
        fserror=FS_OUT_OF_SPACE;
        goto err;
    }

    bzero(&f->inode, sizeof(Inode));
    if(!write_inode(f->d.inode_idx, f->inode)){
        fserror = FS_IO_ERROR;
        goto err;
    }

    strncpy(f->d.name, name, MAX_FILENAME_SIZE-1);
    f->d.name[MAX_FILENAME_SIZE-1] = 0;
    f->d.file_is_open = 1;
    if((d_block = create_directory_entry(f->d)) < 0){
        fserror=FS_OUT_OF_SPACE;
        goto err;
    }
    f->d_block=d_block;

    if(! mark_inode(f->d.inode_idx, 1)){
        fserror = FS_IO_ERROR;
        goto err;
    }

done:
    return f;
err:
    free(f);
    return 0;
}

void close_file(File file){

    fserror=FS_NONE;
    if(!file || ! file->d.file_is_open){
        fserror = FS_FILE_NOT_OPEN;
    }
    else{
        file->d.file_is_open = 0;
        if(! write_directory_entry(file->d, file->d_block)){
            fserror = FS_IO_ERROR;
        }
        bzero(file, sizeof(FileInternals));
        free(file);
        file=NULL;
    }
}

unsigned long read_file(File file, void* buf, unsigned long numbytes){
    unsigned long num_bytes_read = 0;
    uint16_t blocknum;
    uint32_t offset, tocopy;
    uint8_t block[SOFTWARE_DISK_BLOCK_SIZE];

    fserror=FS_NONE;
    if(!file || !file->d.file_is_open){
        fserror=FS_FILE_NOT_OPEN;
        goto done;
    }

    // shouldn't it start from the start of the file
    // what math magic is going on here
    if(file->position + numbytes > file->inode.size){
        numbytes = file->inode.size - file->position;
    }

    while(numbytes > 0){
        if(! read_data_block(file, file->position / SOFTWARE_DISK_BLOCK_SIZE, block)){
            fserror = FS_IO_ERROR;
            goto done;
        }

        offset = file->position % SOFTWARE_DISK_BLOCK_SIZE;
        tocopy = SOFTWARE_DISK_BLOCK_SIZE - offset > numbytes ? numbytes : SOFTWARE_DISK_BLOCK_SIZE - offset;
        memcpy(buf+num_bytes_read, block+offset, tocopy);
        numbytes -= tocopy;
        file->position += tocopy;
        num_bytes_read += tocopy;
    }

done:
    return num_bytes_read;
}

unsigned long write_file(File file, void* buf, unsigned long numbytes){
    unsigned long num_bytes_written = 0;
    uint16_t blocknum;
    uint32_t offset, tocopy;
    uint8_t block[SOFTWARE_DISK_BLOCK_SIZE];

    fserror = FS_NONE;
    if(!file || !file->d.file_is_open){
        fserror = FS_FILE_NOT_OPEN;
        goto done;
    }
    else if(file->mode == READ_ONLY){
        printf("file is read-only!\n");

        fserror = FS_FILE_READ_ONLY;
        goto done;
    }
    else if(file->position + numbytes > MAX_FILE_SIZE){
        fserror = FS_EXCEEDS_MAX_FILE_SIZE;
        goto done;
    }
    else{
        while(numbytes > 0){
            if(!read_data_block(file, file->position / SOFTWARE_DISK_BLOCK_SIZE, block)){
                fserror = FS_IO_ERROR;
                goto done;
            }
        

            offset = file->position % SOFTWARE_DISK_BLOCK_SIZE;
            tocopy = SOFTWARE_DISK_BLOCK_SIZE - offset > numbytes ? numbytes : SOFTWARE_DISK_BLOCK_SIZE - offset;
            memcpy(block+offset, buf+num_bytes_written, tocopy);
            file->inode.size += tocopy;
            //
            //
            if(!write_data_block(file, file->position / SOFTWARE_DISK_BLOCK_SIZE, block)){
                goto done;
            }
            numbytes -= tocopy;
            file->position += tocopy;
            num_bytes_written += tocopy;
        }
    }
done:
    return num_bytes_written;
}

int seek_file(File file, unsigned long bytepos){
    fserror = FS_NONE;
    if(! file){
        fserror = FS_FILE_NOT_OPEN;
        return 0;
    }
    else{
        //
        //
        if(bytepos < MAX_FILE_SIZE){
            fserror=FS_NONE;
            file->position=bytepos;
            if(file->position > file->inode.size){
                file->inode.size=bytepos;
                return write_inode(file->d.inode_idx, file->inode);
            }
        }
        else{
            fserror = FS_EXCEEDS_MAX_FILE_SIZE;
            return 0;
        }
    }
    return 1;

}

unsigned long file_length(File file){
    fserror=FS_NONE;
    return file->inode.size;
}

int delete_file(char* name){
    DirEntry d;
    int32_t block;

    fserror = FS_NONE;
    if((block = find_directory_entry(name, &d)) < 0){
        fserror=FS_FILE_NOT_FOUND;
        return 0;
    }
    else if(d.file_is_open){
        fserror = FS_FILE_OPEN;
        return 0;
    }
    else{
        return delete_directory_entry(d, block);
    }
}

int file_exists(char* name){
    DirEntry d;

    fserror = FS_NONE;
    return find_directory_entry(name, &d) >= 0;
}

void fs_print_error(void){
    switch(fserror){
        case FS_NONE:
            printf("No error.\n");
            break;
        case FS_OUT_OF_SPACE:
            printf("Disk space exhausted.\n");
            break;
        case FS_FILE_NOT_OPEN:
            printf("File not open.\n");
            break;
        case FS_FILE_OPEN:
            printf("File is open.\n");
            break;
        case FS_FILE_NOT_FOUND:
            printf("File not found.\n");
            break;
        case FS_FILE_READ_ONLY:
            printf("File access mode is read-only.\n");
            break;
        case FS_FILE_ALREADY_EXISTS:
            printf("File already exists.\n");
            break;
        case FS_EXCEEDS_MAX_FILE_SIZE:
            printf("Operation exceeds maximun file size.\n");
            break;
        case FS_ILLEGAL_FILENAME:
            printf("Illegal filename.\n");
            break;
        case FS_IO_ERROR:
            printf("I/O error. Something very bad happened.\n");
            break;
        default:
            printf("Unknown error %d\n", fserror);
            break;
    }
}

FSError fserror;

void check_structure_alignment(void){
    printf("Expecting sizeof(Inode)=32, actual = %lu\n", sizeof(Inode));
    printf("Expecting sizeof(IndirectBlock)=512, actual=%lu\n", sizeof(IndirectBlock));
    printf("Expecting sizeof(InodeBlock)=512, actual = %lu\n", sizeof(DirEntry));
    printf("Expecting sizeof(FreeBitmap)=512, actual = %lu\n", sizeof(FreeBitmap));
}
