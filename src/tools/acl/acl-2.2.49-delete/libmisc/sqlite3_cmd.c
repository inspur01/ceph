/*
  File: sqlite3_smd.c
  Add by Lvy
*/


#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "walk_tree.h"
#include "sqlite3_cmd.h"


/*整形转字符串*/
static char* pidtoa( pid_t src, char *dst)
{
	pid_t power, tmp_src;
	tmp_src = src;
	for( power = 1; tmp_src >= 10; tmp_src /= 10 )
		power *= 10;
	for( ; power > 0; power /= 10 ) {
		*dst++ = '0' + src/power;
		src %= power;
	}
	*dst = '\0';
	return dst;
}

static char *rowidtoa( sqlite3_int64 src, char *dst )
{
	
	sqlite3_int64 power, tmp_src;
	tmp_src = src;
	for( power = 1; tmp_src >= 10; tmp_src /= 10 )
		power *= 10;
	for( ; power > 0; power /= 10 ) {
		*dst++ = '0' + src/power;
		src %= power;
	}
	*dst = '\0';
	return dst;
}

/*
**判断是否存在该Id的记录?
**return  0  :  exist
**return  1  :  not exist
**return -1  :  failed
*/
int hasRecord( sqlite3_int64 rec_rowid, sqlite3 *sqlite3_db )
{
	char *errmsg = NULL;
	char **dbResult;
	char str_rowid[32];
	int nRow, nColumn = 0;
	int sqlret;
	rowidtoa(rec_rowid, str_rowid);
	char sqlite3_select_acl[255] = "select * from acl where Id = ";
	strcat(sqlite3_select_acl, str_rowid);
	strcat(sqlite3_select_acl, ";");
	DB_LOCK();
	sqlret = sqlite3_get_table( sqlite3_db, sqlite3_select_acl, &dbResult, &nRow, &nColumn, &errmsg);
	DB_UNLOCK();
	if( sqlret != SQLITE_OK ) {				
		fprintf(stderr, "Select from acl item by Id Failed: %s",sqlite3_errmsg(sqlite3_db));
		return -1;	
	}
	if( (nColumn != 0) && (strcmp(dbResult[nColumn], str_rowid) == 0) ) 
		/* 第一个字段值 */
		return 0;
	else 	
		return 1;
		
}



/*清空Table*/
int clearTable( const char *tableName, sqlite3 *sqlite3_db )
{
	int sqlret;
	char *ErrMsg = 0;
	char sqlite3_clear_acl[255] = "delete from ";
	strcat(sqlite3_clear_acl, tableName);
	strcat(sqlite3_clear_acl, ";");
	/*清除表*/
	DB_LOCK();
	sqlret = sqlite3_exec(sqlite3_db, sqlite3_clear_acl, 0, 0, &ErrMsg);
	DB_UNLOCK();
	if( sqlret != SQLITE_OK ) {
		fprintf(stderr,"Clear Table %s Failed: %s\n",tableName,ErrMsg);
		sqlite3_free(ErrMsg);
		return -1;
	}
	/*自增列归零*/
	char sqlite3_sequence_reset[255] = "update sqlite_sequence set seq=0 where name = '";
	strcat(sqlite3_sequence_reset, tableName);
	strcat(sqlite3_sequence_reset, "';");
	DB_LOCK();
	sqlite3_exec(sqlite3_db, sqlite3_sequence_reset, 0, 0, &ErrMsg);
	DB_UNLOCK();

	return 0;
	
}



/*调用sqlite3保存时间、UID、命令操作、目录路径*/
sqlite3_int64 sqlite3_store_acl(const char *path, char *argv[],int optind, int last_opt, sqlite3 *sqlite3_db)
{
	sqlite3_int64 last_rowid;
	char opt_command[255] = {0};
	char str_pid[32] = {0};
	char *path_end;
	char *ErrMsg = 0;
	int i;
	int sqlret;
	
	/*
	**处理一个setfacl命令的多条操作记录
	**support like setxacl -m user:user1:rwx file1 -m user:user2:rwx file2.
	*/
	if( 0 != last_opt ) {
		strcat(opt_command, argv[0]);
		path_end = strchr(opt_command, 0);
		*(path_end++) = ' ';
		*path_end = '\0';
	}
	for(i = last_opt; i < optind; i++) {
		
		strcat(opt_command, argv[i]);
		path_end = strchr(opt_command, 0);
		*(path_end++) = ' ';
		*path_end = '\0';
	}
	
	/*创建ACL表*/
	char *sqlite3_create_acl = "create table if not exists acl([Id] integer PRIMARY KEY AUTOINCREMENT, [CurPid] INT, [Command] varchar(512), [Path] varchar(255), [State] varchar(32),[DateTime] TimeStamp NOT NULL DEFAULT(datetime('now','localtime')));";
	DB_LOCK();
	sqlret = sqlite3_exec(sqlite3_db, sqlite3_create_acl, 0, 0, &ErrMsg);	
	DB_UNLOCK();
	if( sqlret != SQLITE_OK ) {				
		fprintf(stderr, "Create acl database Failed : %s\n", ErrMsg);
		sqlite3_free(ErrMsg);	
	}
	/*插入ACL数据*/
	pid_t cur_pid = getpid();
	pidtoa(cur_pid, str_pid);
	char sqlite3_insert_acl[255] = "insert into acl([CurPid],[Command],[Path],[State]) values(";
	strcat(sqlite3_insert_acl, str_pid);
	strcat(sqlite3_insert_acl, ",'");
	strcat(sqlite3_insert_acl, opt_command);
	strcat(sqlite3_insert_acl, "','");
	strcat(sqlite3_insert_acl, path);
	strcat(sqlite3_insert_acl, "','Processing');");
	DB_LOCK();
	sqlret = sqlite3_exec(sqlite3_db, sqlite3_insert_acl, 0, 0, &ErrMsg);
	DB_UNLOCK();
	if( sqlret != SQLITE_OK ) {		
		fprintf(stderr, "Insert acl database Failed : %s\n", ErrMsg);
		sqlite3_free(ErrMsg);	
	}
	DB_LOCK();
	last_rowid = sqlite3_last_insert_rowid(sqlite3_db);
	DB_UNLOCK();

	return last_rowid;
}



/*更新State状态*/
int sqlite3_change_acl_state(sqlite3_int64 change_rowid, int new_state, sqlite3 *sqlite3_db)
{
	int sqlret;
	char str_rowid[32];
	rowidtoa(change_rowid, str_rowid);
	char *ErrMsg = 0;
	char sqlite3_cmd_succeed[255] = "update acl set State = 'succeed' where Id = ";
	char sqlite3_cmd_failed[255] = "update acl set State = 'failed' where Id = ";
	strcat(sqlite3_cmd_succeed, str_rowid);
	strcat(sqlite3_cmd_succeed, ";");
	strcat(sqlite3_cmd_failed, str_rowid);
	strcat(sqlite3_cmd_failed, ";");
	/*执行操作*/
	switch( new_state ) {
		case 0:
			DB_LOCK();
			sqlret = sqlite3_exec(sqlite3_db, sqlite3_cmd_succeed, 0, 0, &ErrMsg);
			DB_UNLOCK();
			if( sqlret != SQLITE_OK ) {		
				fprintf(stderr, "Update State Failed : %s\n", ErrMsg);
				sqlite3_free(ErrMsg);
				return 1;
			}	
			break;
		case 1:
			DB_LOCK();
			sqlret = sqlite3_exec(sqlite3_db, sqlite3_cmd_failed, 0, 0, &ErrMsg);
			DB_UNLOCK();
			if( sqlret != SQLITE_OK ) {		
				fprintf(stderr, "Update State Failed : %s\n", ErrMsg);
				sqlite3_free(ErrMsg);
				return 1;
			}	
			break;
		default:
			fprintf(stderr, "Update State Failed! Unrecognized Parameter!\n");
		return -1;	
	}
	
	return 0;
}



int sqlite3_change_acl_path(sqlite3_int64 change_rowid, const char *current_path, sqlite3 *sqlite3_db)
{
	int sqlret;
	char str_rowid[32];
	char *ErrMsg = 0;
	rowidtoa(change_rowid, str_rowid);
	char sqlite3_change_path[4096] = "update acl set Path = '";
	strcat(sqlite3_change_path, current_path);
	strcat(sqlite3_change_path, "' where Id = ");
	strcat(sqlite3_change_path, str_rowid);
	strcat(sqlite3_change_path, ";");
	DB_LOCK();
	sqlret = sqlite3_exec(sqlite3_db, sqlite3_change_path, 0, 0, &ErrMsg);
	DB_UNLOCK();
	if( sqlret != SQLITE_OK ) {	
		if( strcpy(ErrMsg, "database is locked") != 0 ) {
			fprintf(stderr, "Update acl Path Failed : %s\n", ErrMsg);
			sqlite3_free(ErrMsg);
			return 1;
		}
	}	

	return 0;
}


/*根据键值删除ACL条目*/
int sqlite3_delete_acl_item( sqlite3_int64 delete_rowid, sqlite3 *sqlite3_db )
{
	int sqlret;
	char str_rowid[32];
	rowidtoa(delete_rowid, str_rowid);
	char *ErrMsg = 0;
	char sqlite3_delete_acl[255] = "delete from acl where Id = ";
	strcat(sqlite3_delete_acl, str_rowid);
	strcat(sqlite3_delete_acl, ";");
	/*删除ACL*/
	DB_LOCK();
	sqlret = sqlite3_exec(sqlite3_db, sqlite3_delete_acl, 0, 0, &ErrMsg);
	DB_UNLOCK();
	if( sqlret != SQLITE_OK ) {
		fprintf(stderr,"Delete acl item Failed: %s\n",ErrMsg);
		sqlite3_free(ErrMsg);
		return 1;
	}
	
	return 0;
}


/*打印ACL表*/
int sqlite3_list_table( const char *tableName, sqlite3 *sqlite3_db )
{
	char *ErrMsg = 0;
	char sqlite3_list_acl[255] = "select * from ";
	char **dbResult;
	strcat(sqlite3_list_acl, tableName);
	strcat(sqlite3_list_acl, ";");
	int sqlret;
	int i, j;
	int index;
	int nRow = 0; 
	int	nColumn = 0;
	int Command_Col = -1; 
	int DateTime_Col = -1;
	int Path_Col = -1;
	DB_LOCK();
	sqlret = sqlite3_get_table(sqlite3_db, sqlite3_list_acl, &dbResult, &nRow, &nColumn, &ErrMsg);
	DB_UNLOCK();
	if( sqlret != SQLITE_OK ) {				
		fprintf(stderr, "Getxacl -L , Get acl tables Failed: %s",sqlite3_errmsg(sqlite3_db));
		return -1;	
	}
	if( 0 != nColumn ) {
		/*输出表头*/
		for( j = 0;j < nColumn; j++ ) {
			
			if( (strcmp(dbResult[j], "Command") == 0) ) {
				fprintf(stdout,"%-42.42s",dbResult[j]);
				Command_Col = j;
			}else if( (strcmp(dbResult[j], "Path") == 0) ) {
				fprintf(stdout,"%-72.72s",dbResult[j]);
				Path_Col = j;
			}else if( (strcmp(dbResult[j], "DateTime") == 0) ) {
				fprintf(stdout,"%-28.28s",dbResult[j]);
				DateTime_Col = j;
			}else {	
				fprintf(stdout,"%-14.14s",dbResult[j]);
			}	
		}
		fprintf(stdout,"\n");
		/*输出数据*/
		index = nColumn;
		for( i = 0;i < nRow; i++ ) {
			for( j = 0;j < nColumn; j++ ) {
				/*
				**当不存在"Command"和"DateTime"列时，(Command_Col = -1) And (DateTime_Col = -1),
				**index == ((i + 1)*nColumn + Command_Col)和index == ((i + 1)*nColumn + DateTime_Col)
				**Never true!确保和表头对齐!
				*/
				if( index == ((i + 1)*nColumn + Command_Col) )
					fprintf(stdout,"%-42.42s",dbResult[index]);
				else if( index == ((i + 1)*nColumn + Path_Col) )
					fprintf(stdout,"%-72.72s",dbResult[index]);
				else if( index == ((i + 1)*nColumn + DateTime_Col) )
					fprintf(stdout,"%-28.28s",dbResult[index]);
				else
					fprintf(stdout,"%-14.14s",dbResult[index]);
				++index;
			}
			fprintf(stdout,"\n");
		}
	}
	DB_LOCK();
	sqlite3_free_table( dbResult);
	DB_UNLOCK();
	return 0;
}


