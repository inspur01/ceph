/******************************************************************************
 
                   ��Ȩ���� (C), 2015-2099, �˳�������Ϣ��ҵ�ɷ����޹�˾
 
  ******************************************************************************
   �� �� ��   : UserFileQuota.cc
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
#include <errno.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <map>

using namespace std;

#include "common/UserFileQuota.h"
#include "common/config.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix _prefix(_dout)
static ostream& _prefix(std::ostream *_dout) {
  return *_dout << "UserFileQuota ";
}

void UserFileQuota::update(int32_t vary_cnt)
{
  user_file_cnt += vary_cnt;
}

