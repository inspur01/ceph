/******************************************************************************
 
                   ��Ȩ���� (C), 2015-2099, �˳�������Ϣ��ҵ�ɷ����޹�˾
 
  ******************************************************************************
   �� �� ��   : GrpQuotaData.cc
   �� �� ��   : ����
   ��    ��   : lvqiang
   ��������   : 2016��5��16��
   ����޸�   :
   ��������   : �û������������
   �����б�   :
   �޸���ʷ   :
   1.��  ��   : 2016��5��16��
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

#include "common/GrpQuotaData.h"
#include "common/config.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix _prefix(_dout)
static ostream& _prefix(std::ostream *_dout) {
  return *_dout << "GrpQuotaData ";
}

void GrpQuotaData::modify(GrpQuotaData& grp_quota)
{
  hardlimit = grp_quota.hardlimit;
  softlimit = grp_quota.softlimit;    
  accuracy = grp_quota.accuracy;
}

