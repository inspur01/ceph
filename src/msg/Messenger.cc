
#include "include/types.h"
#include "Messenger.h"

#include "msg/simple/SimpleMessenger.h"
#include "msg/async/AsyncMessenger.h"
#include "msg/simple/SimpleMessenger.h"
#ifdef HAVE_XIO
#include "msg/xio/XioMessenger.h"
#endif

Messenger *Messenger::create(CephContext *cct, const string &type,
			     entity_name_t name, string lname,
			     uint64_t nonce, uint64_t features)
{
  if (cct->_conf->ms_type == "simple")
    return new SimpleMessenger(cct, name, lname, nonce, features);
  else if (cct->_conf->ms_type == "async")
    return new AsyncMessenger(cct, name, lname, nonce);
#ifdef HAVE_XIO
  else if (type == "xio")
    return new XioMessenger(cct, name, lname, nonce, features);
#endif
  lderr(cct) << "unrecognized ms_type '" << cct->_conf->ms_type << "'" << dendl;
  return NULL;
}

/*
 * Pre-calculate desired software CRC settings.  CRC computation may
 * be disabled by default for some transports (e.g., those with strong
 * hardware checksum support).
 */
int Messenger::get_default_crc_flags(md_config_t * conf)
{
  int r = 0;
  if (conf->ms_crc_data)
    r |= MSG_CRC_DATA;
  if (conf->ms_crc_header)
    r |= MSG_CRC_HEADER;
  return r;
}
