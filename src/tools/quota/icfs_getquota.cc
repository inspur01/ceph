#define _FILE_OFFSET_BITS 64
#define __USE_FILE_OFFSET64

#include <sys/types.h>
#include <attr/xattr.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <string.h>
using namespace std;
//#include "client/Client.h"
#define XATTR_NOATTR   0
#define XATTR_ERROR   -1
#define XATTR_SUCCESS  1
#define LOG_TYPE      0
#define LOG_ERROR     1
#define LOG_INFO      2
#define LOG_DEBUG     3
#define LOG_JOURNAL   4
#define LOG_DEDUP     5
#define XATTR_MAX_SIZE 4096


void usage()
{
    char appname[]="icfs-quota";
    char **argv = (char **) malloc(2 * sizeof(char *));
    argv[0] = appname;
    argv[1] = (char *) malloc(3 * sizeof(char));
    memcpy(argv[1], "-h\0", 3);
    printf("\n"
           "-------------------------------------------------------\n"
           "icfs get quota!\n"
           "\n"
           "Usage: icfs-getquota [/getquota_directory_path] \n"
           "\n"
           "Example :\n  icfs-getquota /mnt/icfs/test1/ \n"
           "-------------------------------------------------------\n");
    exit(0);
}


int read_xattr_value(const char *opath, const char* xattr_name, char* xattr_value, unsigned int xattr_value_size){
    int ret = lgetxattr(opath, xattr_name, xattr_value, xattr_value_size);
    int errno_t=errno;
    // char log_buf[256];    
    // sprintf(log_buf,"<%-10s> path:%s xattr_name:%s xattr_value:%s ret:%d","read_ea",opath,xattr_name,xattr_value,ret);
     //cout<<"icfs-setquota["<<LOG_DEBUG<<"]:"<<log_buf<<std::endl; 
    if(ret<0 && errno_t != ENODATA){
        cout << "icfs-getquota: "<<opath<<": "<<strerror(errno_t)<<std::endl;
        return XATTR_ERROR;
    }else if(ret<0 && errno_t==ENODATA){
        cout <<opath<<": "<<xattr_name<<":  No such attribute"<<std::endl;
        return ret;
    }
    if(ret>0)
        xattr_value[ret]='\0';
    //cout <<"icfs-setquota["<<LOG_DEBUG<<"]: read_xattr_value SUCCESS [opath:"<<opath<<"][name:"<<xattr_name<<"][value:"<<xattr_value<<std::endl;
    
    return XATTR_SUCCESS;
}


int main(int argc, char *argv[])
{
    char path_name[XATTR_MAX_SIZE];
    char xattr_name[XATTR_MAX_SIZE];
    char xattr_value[XATTR_MAX_SIZE];
    int res;
    memset(xattr_value, 0x00, sizeof(xattr_value));
    
    if ((argc > 1) && (strcmp(argv[1], "-h") == 0)) {
        usage();
        return -1;
    }
	
    if (argc < 2 || argc > 2 ) {
		printf("Parameter is not valid !");
		usage();
        return -1;
    }
    
	
    strcpy(path_name, argv[1]);
    strcpy(xattr_name,"icfs.quota.max_Gbytes");
     
    
    res = read_xattr_value(path_name,xattr_name, xattr_value, XATTR_MAX_SIZE);
    if(res < 0)
    {
	   return -1;	
	}
    printf("dir:%s \n",path_name);
    printf("icfs.quota: \"max_Gbytes=%s\" \n", xattr_value);

    return 0;
    
}