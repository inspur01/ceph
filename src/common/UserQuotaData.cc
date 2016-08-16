/******************************************************************************
 
                   ��Ȩ���� (C), 2015-2099, �˳�������Ϣ��ҵ�ɷ����޹�˾
 
  ******************************************************************************
   �� �� ��   : UserQuotaData.cc
   �� �� ��   : ����
   ��    ��   : lvqiang
   ��������   : 2016��3��23��
   ����޸�   :
   ��������   : �û����������
   �����б�   :
   �޸���ʷ   :
   1.��  ��   : 2016��3��23��
     ��  ��   : lvqiang
   �޸�����   : �����ļ�
 
 ******************************************************************************/
#include <errno.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <map>

using namespace std;

#include "common/UserQuotaData.h"
#include "common/config.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix _prefix(_dout)
static ostream& _prefix(std::ostream *_dout) {
  return *_dout << "UserQuotaData ";
}

void UserQuotaData::modify(UserQuotaData& user_quota)
{
  hardlimit = user_quota.hardlimit;
  softlimit = user_quota.softlimit;    
  accuracy = user_quota.accuracy;
}

