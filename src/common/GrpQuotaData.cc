/******************************************************************************
 
                   版权所有 (C), 2015-2099, 浪潮电子信息产业股份有限公司
 
  ******************************************************************************
   文 件 名   : GrpQuotaData.cc
   版 本 号   : 初稿
   作    者   : lvqiang
   生成日期   : 2016年5月16日
   最近修改   :
   功能描述   : 用户组配额数据类
   函数列表   :
   修改历史   :
   1.日  期   : 2016年5月16日
     作  者   : lvqiang
   修改内容   : 创建文件
 
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

