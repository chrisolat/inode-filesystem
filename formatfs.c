
#include <stdio.h>
#include "softwaredisk.h"
#include "filesystem.h"
int main(){

    int res = init_software_disk();
    char* name = "file1.txt";
    File f = create_file(name);
    char* data = "data to write";
    write_file(f, data, sizeof(data));
    seek_file(f, 0);
    char read_data[sizeof(data)+1];
    read_file(f,read_data,sizeof(data));
    fs_print_error();
    printf("%s\n", read_data);
    return 0;
}