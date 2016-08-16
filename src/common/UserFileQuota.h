/******************************************************************************
 
                   ��Ȩ���� (C), 2015-2099, �˳�������Ϣ��ҵ�ɷ����޹�˾
 
  ******************************************************************************
   �� �� ��   : UserFileQuota.h
   �� �� ��   : ����
   ��    ��   : lvqiang
   ��������   : 2016��4��20��
   ����޸�   :
   ��������   : �û��ļ������
   �����б�   :
   �޸���ʷ   :
   1.��  ��   : 2016��4��20��
     ��  ��   : lvqiang
   �޸�����   : �����ļ�
 
 ******************************************************************************/
#ifndef CEPH_COMMON_USER_FILE_QUOTA_H
#define CEPH_COMMON_USER_FILE_QUOTA_H

#include "include/types.h"
#include "include/filepath.h"
#include "include/elist.h"

#include "include/Context.h"

#include "include/buffer.h"

static const version_t FILE_QUOTA_VERSION = 100;


/*
 * UserFileQuota
 */
class UserFileQuota {
  public:
    version_t version;
    uint32_t user_file_cnt; // �û��ļ���
    int32_t vary_cnt;       // �仯��
    uid_t uid;
    int32_t vary_oldcnt;
  public:	
	UserFileQuota() : version(FILE_QUOTA_VERSION), user_file_cnt(0), vary_cnt(0),vary_oldcnt(0) {}
	UserFileQuota(uid_t tuid) : version(FILE_QUOTA_VERSION), user_file_cnt(0), vary_cnt(0), uid(tuid),vary_oldcnt(0) {}

	void SetUid(uid_t tuid) 
	{
	  uid = tuid;
	}

	uid_t GetUid() 
	{
	  return uid;
	}
	
    void encode(bufferlist &bl) const 
	{
      ::encode(version, bl);
      ::encode(user_file_cnt, bl);
	  ::encode(vary_cnt, bl);
	  ::encode(uid, bl);
	}

    void decode(bufferlist::iterator& bl) 
	{
	  ::decode(version, bl);
	  ::decode(user_file_cnt, bl);
	  ::decode(vary_cnt, bl);
	  ::decode(uid, bl);
	}

    void update(int32_t vary_cnt);
    void dump(Formatter *f) const 
	{
      f->dump_unsigned("version", version);
      f->dump_unsigned("user_file_cnt", user_file_cnt);
      f->dump_unsigned("vary_cnt", vary_cnt);
      f->dump_unsigned("uid", uid);
    }

    static void generate_test_instances(list<UserFileQuota*>& o) 
	{
      //o.push_back(new UserFileQuota());
    }
};
WRITE_CLASS_ENCODER(UserFileQuota);

#endif 

