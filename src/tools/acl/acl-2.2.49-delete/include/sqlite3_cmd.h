#ifndef __SQLITE3_CMD_H
#define __SQLITE3_CMD_H

#include "sqlite3.h"
#include "proc_flock.h"

/*数据库路径名*/
#define SQLITE3_DATABASE_NAME  "/etc/icfs/.AclDataBase.db"
/*表名*/
#define SQLITE3_TABLE_NAME "acl"

#define DB_OPEN(db)		do {	\
	if( sqlite3_open(SQLITE3_DATABASE_NAME, &(db)) != SQLITE_OK ) {	\
		fprintf(stderr, "Can not open sqlite3_database: %s",sqlite3_errmsg(db)); \
		return -1;	\
	} \
} while (0)	

#define DB_CLOSE(db)	do { \
	sqlite3_close(db);	\
	(db) = 0; \
} while (0)	


static char* pidtoa( pid_t src, char *dst );
static char *rowidtoa( sqlite3_int64 src, char *dst );
int hasRecord( sqlite3_int64 rec_rowid, sqlite3 *sqlite3_db );
int clearTable( const char *tableName, sqlite3 *sqlite3_db );
int sqlite3_change_acl_state(sqlite3_int64 change_rowid, int new_state, sqlite3 *sqlite3_db);
int sqlite3_change_acl_path(sqlite3_int64 change_rowid, const char *current_path, sqlite3 *sqlite3_db);
int sqlite3_delete_acl_item( sqlite3_int64 delete_rowid, sqlite3 *sqlite3_db );
int sqlite3_list_table( const char *tableName, sqlite3 *sqlite3_db );
sqlite3_int64 sqlite3_store_acl(const char *path, char *argv[],int optind, int last_opt, sqlite3 *sqlite3_db);







#endif 

