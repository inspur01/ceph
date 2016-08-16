#ifndef CEPH_MCLIENTQUOTA_H
#define CEPH_MCLIENTQUOTA_H

#include "msg/Message.h"
#include "common/UserQuotaData.h"
#include "common/UserFileQuota.h"
#include "common/GrpQuotaData.h"
#include "common/GrpFileQuota.h"
#include "common/DirQuotaData.h"

struct MClientQuota : public Message {
  inodeno_t ino;
  nest_info_t rstat;
  quota_info_t quota;

  MClientQuota() :
    Message(CEPH_MSG_CLIENT_QUOTA),    //zquota_12 all the file
    ino(0)
  {
    memset(&rstat, 0, sizeof(rstat));
    memset(&quota, 0, sizeof(quota));
  }
private:
  ~MClientQuota() {}

public:
  const char *get_type_name() const { return "client_dir_quota"; }
  void print(ostream& out) const {
    out << "client_dir_quota(";
    out << " [" << ino << "] ";
    out << rstat;
    out << ")";
  }

  void encode_payload(uint64_t features) {
    ::encode(ino, payload);
    ::encode(rstat.rctime, payload);
    ::encode(rstat.rbytes, payload);
    ::encode(rstat.rfiles, payload);
    ::encode(rstat.rsubdirs, payload);
    ::encode(quota, payload);
  }
  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    ::decode(ino, p);
    ::decode(rstat.rctime, p);
    ::decode(rstat.rbytes, p);
    ::decode(rstat.rfiles, p);
    ::decode(rstat.rsubdirs, p);
    ::decode(quota, p);
    assert(p.end());
  }
};

struct MClientUserQuota : public Message {
  map<uid_t, UserQuotaData> user_quota_map;
  UserQuotaData user_quota_info;
  uint64_t opcode; 

  MClientUserQuota() :
  Message(CEPH_MSG_CLIENT_USER_QUOTA),
		opcode(0)
  {
    memset(&user_quota_info, 0, sizeof(user_quota_info));
  }
  MClientUserQuota(uint64_t op) :
    Message(CEPH_MSG_CLIENT_USER_QUOTA),
	opcode(op)
	{
		memset(&user_quota_info, 0, sizeof(user_quota_info));
		memset(&user_quota_map, 0, sizeof(user_quota_map));
	}

private:
  ~MClientUserQuota() {}

public:
  const char *get_type_name() const { return "client_user_quota"; }
  void print(ostream& out) const {
    out << "client_user_quota(";
    out << user_quota_info.uid;
	out << user_quota_info.hardlimit;
	out << user_quota_info.softlimit;
	out << user_quota_info.used_bytes;
	out << user_quota_info.buf_used;
    out << ")";
  }

  void encode_payload(uint64_t features) {
	::encode(opcode, payload);
	::encode(user_quota_info, payload);
	if(CEPH_MSG_CLIENT_USER_QUOTA_UPDATE == opcode) {
	  ::encode(user_quota_map, payload);
	}
  }
  void decode_payload() {
    bufferlist::iterator p = payload.begin();
	::decode(opcode, p);
	::decode(user_quota_info, p);
	if(CEPH_MSG_CLIENT_USER_QUOTA_UPDATE == opcode) {
	  ::decode(user_quota_map, p);
	}
    //assert(p.end());
  }
};

struct MClientUserFileQuota : public Message {
	map<uid_t, UserFileQuota> user_quota_map;

	MClientUserFileQuota() : Message(CEPH_MSG_CLIENT_USER_FILE_QUOTA)
	{
		memset(&user_quota_map, 0, sizeof(user_quota_map));
	}

private:
	~MClientUserFileQuota() {}

public:
	const char *get_type_name() const { return "client_user_file_quota"; }
	void print(ostream& out) const {
		out << "client_user_file_quota(";
		out << ")";
	}

	void encode_payload(uint64_t features) {
		::encode(user_quota_map, payload);
	}
	void decode_payload() {
		bufferlist::iterator p = payload.begin();
		::decode(user_quota_map, p);
		//assert(p.end());
	}
};

struct MClientGrpQuota : public Message {
  map<uid_t, GrpQuotaData> group_quota_map;
  GrpQuotaData group_quota_info;
  uint64_t opcode; 

  MClientGrpQuota() :
  Message(CEPH_MSG_CLIENT_GRP_QUOTA),
		opcode(0)
  {
    memset(&group_quota_info, 0, sizeof(group_quota_info));
  }
  MClientGrpQuota(uint64_t op) :
    Message(CEPH_MSG_CLIENT_GRP_QUOTA),
	opcode(op)
	{
		memset(&group_quota_info, 0, sizeof(group_quota_info));
		memset(&group_quota_map, 0, sizeof(group_quota_map));
	}

private:
  ~MClientGrpQuota() {}

public:
  const char *get_type_name() const { return "client_group_quota"; }
  void print(ostream& out) const {
    out << "client_group_quota(";
    out << group_quota_info.gid;
	out << group_quota_info.hardlimit;
	out << group_quota_info.softlimit;
	out << group_quota_info.used_bytes;
	out << group_quota_info.buf_used;
    out << ")";
  }

  void encode_payload(uint64_t features) {
	::encode(opcode, payload);
	::encode(group_quota_info, payload);
	if(CEPH_MSG_CLIENT_GRP_QUOTA_UPDATE == opcode) {
	  ::encode(group_quota_map, payload);
	}
  }
  void decode_payload() {
    bufferlist::iterator p = payload.begin();
	::decode(opcode, p);
	::decode(group_quota_info, p);
	if(CEPH_MSG_CLIENT_GRP_QUOTA_UPDATE == opcode) {
	  ::decode(group_quota_map, p);
	}
    //assert(p.end());
  }
};

struct MClientGrpFileQuota : public Message {
	map<gid_t, GrpFileQuota> group_file_map;

	MClientGrpFileQuota() : Message(CEPH_MSG_CLIENT_GRP_FILE_QUOTA)
	{
		memset(&group_file_map, 0, sizeof(group_file_map));
	}

private:
	~MClientGrpFileQuota() {}

public:
	const char *get_type_name() const { return "client_group_file_quota"; }
	void print(ostream& out) const {
		out << "client_group_file_quota(";
		out << ")";
	}

	void encode_payload(uint64_t features) {
		::encode(group_file_map, payload);
	}
	void decode_payload() {
		bufferlist::iterator p = payload.begin();
		::decode(group_file_map, p);
		//assert(p.end());
	}
};

struct MClientDirQuota : public Message {
  map<inodeno_t, DirQuotaData> dir_quota_map;
  DirQuotaData dir_quota_info;
  uint64_t opcode;

  MClientDirQuota() :
    Message(CEPH_MSG_CLIENT_DIR_QUOTA),opcode(0){}

  MClientDirQuota(uint64_t op) :
    Message(CEPH_MSG_CLIENT_DIR_QUOTA),
    opcode(op)
  {
    memset(&dir_quota_info, 0, sizeof(dir_quota_info));
    memset(&dir_quota_map, 0, sizeof(dir_quota_map));
  }
private:
  ~MClientDirQuota() {}

public:
  const char *get_type_name() const { return "client_dir_quota"; }
  void print(ostream& out) const {
    out << "client_dir_quota(";
    out << ")";
  }

  void encode_payload(uint64_t features) {
    ::encode(opcode, payload);
    ::encode(dir_quota_info, payload);
    if (CEPH_MSG_CLIENT_DIR_QUOTA_UPDATE == opcode)
      ::encode(dir_quota_map, payload);

  }
  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    ::decode(opcode, p);
    ::decode(dir_quota_info, p);
    if (CEPH_MSG_CLIENT_DIR_QUOTA_UPDATE == opcode)
      ::decode(dir_quota_map, p);

    //assert(p.end());
  }
};

#endif
