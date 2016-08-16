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
#define DIR_PATH      256

void usage()
{
    char appname[]="icfs-quota";
    char **argv = (char **) malloc(2 * sizeof(char *));
    argv[0] = appname;
    argv[1] = (char *) malloc(3 * sizeof(char));
    memcpy(argv[1], "-h\0", 3);
    printf("\n"
           "-------------------------------------------------------\n"
           "icfs fs thin provisoning!\n "
           "\n"
           "Usage: icfs-setthin [/setthin_directory_path] [size_value] \n"
           "\n"
           "Example :\n  icfs-setthin /mnt/icfs/test1/ 1000\n"
           "-------------------------------------------------------\n");
    exit(0);
}

int write_xattr_value(const char *opath,const char* xattr_name,const char* xattr_value){
    int ret=lsetxattr(opath, xattr_name, xattr_value, strlen(xattr_value), 0);
    int errno_t=errno;

    if(ret<0){
       char log_buf[256];    
       sprintf(log_buf,"write_xattr_value error [ipath:%s] [value:%s] [errno:%d :%s]",opath, xattr_value, errno_t,strerror(errno_t));
       //sprintf(log_buf,"write_xattr_value error [ipath:%s][name:%s][value:%s][ret:%d][errno:%d][strerror:%s]",opath,xattr_name,xattr_value,ret,errno_t,strerror(errno_t));
       //cout<<"icfs-setthin["<<LOG_ERROR<<"]:"<<log_buf<<std::endl; 
       cout<<"icfs-setthin: "<< opath <<": "<< strerror(errno_t) << std::endl; 
       return ret;
    }
    //cout << "icfs-setquota["<<LOG_DEBUG<<"]: write_xattr_value SUCCESS [ipath:"<<opath<<"] [name:"<<xattr_name<<"][value:"<<xattr_value<<std::endl;
    return 0;
}



int read_xattr_value(const char *opath, const char* xattr_name, char* xattr_value, unsigned int xattr_value_size){
    int ret = lgetxattr(opath, xattr_name, xattr_value, xattr_value_size);
    int errno_t=errno;
    // char log_buf[256];    
    // sprintf(log_buf,"<%-10s> path:%s xattr_name:%s xattr_value:%s ret:%d","read_ea",opath,xattr_name,xattr_value,ret);
    //cout<<"icfs-setquota["<<LOG_DEBUG<<"]:"<<log_buf<<std::endl; 
    if(ret<0 && errno_t != ENODATA){
        //cout << "icfs-getthin["<<LOG_ERROR<<"]: read_xattr_value error [opath:"<<opath<<"]"<<strerror(errno_t)<<std::endl;
       cout << "icfs-getquota: "<<opath<<": "<<strerror(errno_t)<<std::endl;
       return XATTR_ERROR;
    }else if(ret<0 && errno_t==ENODATA){
        //cout <<"icfs-getthin["<<LOG_DEBUG<<"]: read_xattr_value ENODATA [opath:"<<opath<<"] = 0"<<std::endl;
       cout <<opath<<": "<<xattr_name<<":  No such attribute"<<std::endl;
       return XATTR_NOATTR;
    }
    if(ret>0)
        xattr_value[ret]='\0';
    //cout <<"icfs-setquota["<<LOG_DEBUG<<"]: read_xattr_value SUCCESS [opath:"<<opath<<"][name:"<<xattr_name<<"][value:"<<xattr_value<<std::endl;
    return XATTR_SUCCESS;
}


int main(int argc, char *argv[])
{
   
    char string[DIR_PATH];
    char path_name[XATTR_MAX_SIZE];
    char xattr_name[XATTR_MAX_SIZE];
    char quota_value[XATTR_MAX_SIZE];
    char xattr_value[XATTR_MAX_SIZE];

    int res;
    bool sthin=false;
    bool squota=true;
    memset(xattr_value, 0x00, sizeof(xattr_value));
    
    if ((argc > 1) && (strcmp(argv[1], "-h") == 0)) {
        usage();
        return -1;
    }
    if (argc != 3) {
        printf("Parameter is not valid !");
		usage();
        return -1;
    }
    
    FILE *fp;
    char find_str[]="client_fs_provisioning";
    char find_quota[]="client_dir_quota";
    int line=0;
    int thinnum=0;
    int quotanum=0;
    char file_str[XATTR_MAX_SIZE];
    fp=fopen("/etc/icfs/icfs.conf","r");
    if(fp==NULL)
    {
        cout<<"open icfs.conf for read error!\n";
        return -1;
    }
    while(fgets(file_str,sizeof(file_str),fp))
    {
        line++;
        if(strstr(file_str,find_str))
        {
            if(strstr(file_str,"true")&&file_str[0]!='#' &&file_str[0]!=';')
            {
                sthin=true;
                thinnum++;
            }else if(strstr(file_str,"false")&&file_str[0]!='#' &&file_str[0]!=';') {
                sthin=false;
                thinnum++;
            }
            
            //printf("%s in %d : %s",find_str,line, file_str);
            //fclose(fp);
            
            continue;
        }
        if(strstr(file_str,find_quota))
        {
            if(strstr(file_str,"true")&&file_str[0]!='#' &&file_str[0]!=';'){
                squota=false;
                quotanum++;
            }else if(strstr(file_str,"false")&&file_str[0]!='#' &&file_str[0]!=';'){
                squota=true;
                quotanum++;
            }
            
        }
    }
    fclose(fp);
    if(thinnum > 1)
        printf("warning : client_fs_provisioning redefined \n");
    if(quotanum > 1)
        printf("warning : client_dir_quota redefined \n");
   if(sthin&&squota){
    strcpy(path_name, argv[1]);
    strcpy(quota_value, argv[2]);
    strcpy(xattr_name,"icfs.quota.max_Gbytes");
  
    res =write_xattr_value(path_name,xattr_name,quota_value);
    if(res<0)
    {
        //printf("ERROR 7: set path: [%s] thin: [%s] value: [%s] failed!\n", path_name, xattr_name, quota_value);
        return res;
    }
    res=read_xattr_value(path_name, xattr_name, xattr_value, XATTR_MAX_SIZE);
    if(res < 0)
    {
        return -1;	
	}
    printf("set thin : %s %s \n", path_name, xattr_value);
    printf("set thin value %s SUCCESS!\n", quota_value);
   
    
   }else if(!sthin){
       //fclose(fp);
       cout<<"icfs-setthin: "<<path_name<<": Operation not supported"<<std::endl;
       return -1;
   }else {
       cout<<"client_dir_quota can't be set to true "<<std::endl;
       return -1;
   }
	strcpy(string, "touch ");	
	strcat(string, path_name);
	strcat(string, "/.tmp");
	system(string);
	strcpy(string, "rm -rf ");
	strcat(string, path_name);
	strcat(string, "/.tmp");
	system(string);
   
    return 0;
}



