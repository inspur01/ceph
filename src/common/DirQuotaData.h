#ifndef CEPH_COMMON_DIR_QUOTA_H
#define CEPH_COMMON_DIR_QUOTA_H

#include "include/types.h"
#include "include/filepath.h"
#include "include/elist.h"

#include "include/Context.h"
#include "include/buffer.h"

static const char DIR_QUOTA_UPDATE = 'U';
static const char DIR_QUOTA_SET = 'S';
static const char DIR_QUOTA_DEL = 'D';
/*100M*/
//#define REPORT_SIZE		  (100<<20)


class DirQuotaData {
  public:
    inodeno_t ino;
    uint64_t  used_bytes;
    uint32_t  buf_incused;
		uint32_t  buf_decused;
		uint32_t  buf_oldinc;
		uint32_t  buf_olddec;


    DirQuotaData() 
    {
      ino = 0;
      used_bytes  = 0;
      buf_incused  = 0;
      buf_decused = 0;
      buf_oldinc = 0;
      buf_olddec = 0;

    }

		DirQuotaData(inodeno_t new_ino):ino(new_ino)
    {
      used_bytes  = 0;
      buf_incused = 0;
			buf_decused = 0;
			buf_oldinc = 0;
      buf_olddec = 0;

    }

    void encode(bufferlist &bl) const
    {
      ::encode(ino, bl);
      ::encode(used_bytes, bl);
      ::encode(buf_incused, bl);
			::encode(buf_decused, bl);
			::encode(buf_oldinc, bl);
			::encode(buf_olddec, bl);

    }

    void decode(bufferlist::iterator& bl)
    {
      ::decode(ino, bl);
      ::decode(used_bytes, bl);
      ::decode(buf_incused, bl);
      ::decode(buf_decused, bl);
      ::decode(buf_oldinc, bl);
      ::decode(buf_olddec, bl);

    }

    void dump(Formatter *f) const
    {
      f->dump_unsigned("ino", ino);
      f->dump_unsigned("used_bytes", used_bytes);
      f->dump_unsigned("buf_incused", buf_incused);
      f->dump_unsigned("buf_decused", buf_decused);

    }

    static void generate_test_instances(list<DirQuotaData*>& o)
    {
      //o.push_back(new DirQuotaData());
    }
};
WRITE_CLASS_ENCODER(DirQuotaData);

#endif 
