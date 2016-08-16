//zzg
#include <stdlib.h>
#include <sstream>
#include <pwd.h>
#include <security/pam_appl.h>

#include "common/debug.h"
#include "ClientUid.h"

#define USER_UNKNOWN 10
#define AUTH_ERR 7
#define OTHER_ERR 1
#define AUTH_OK  0

//function used to get user input
int function_conversation(int num_msg, const struct pam_message **msg, struct pam_response **resp, void *appdata_ptr)
{
  if (num_msg != 1 || msg[0]->msg_style != PAM_PROMPT_ECHO_OFF)
    return PAM_CONV_ERR;
  
  struct pam_response *reply;
  
  reply = (struct pam_response *)malloc(sizeof(struct pam_response));
  if (reply == NULL)
    return PAM_CONV_ERR;
  
  reply[0].resp = strdup((char *)appdata_ptr);
  reply[0].resp_retcode = 0;
  
  *resp = reply;
  reply == NULL;
  
  return PAM_SUCCESS;
}

int ClientUid::user_authenticate(const char *username, const char *passwd)
{
  const struct pam_conv local_conversation = { function_conversation, (void *)passwd };
  pam_handle_t *local_auth_handle = NULL; // this gets set by pam_start
  
  int retval;
  
  // local_auth_handle gets set based on the service
  retval = pam_start("ceph-auth", username, &local_conversation, &local_auth_handle);
  
  if (retval != PAM_SUCCESS)
  {
    generic_dout(10) << "pam_start returned " << retval << dendl;
    return -OTHER_ERR;
  }
  
  retval = pam_authenticate(local_auth_handle, 0);
  
  if (retval != PAM_SUCCESS)
  {
    if (retval == PAM_AUTH_ERR)
    {
      generic_dout(10) << "Authentication failure." << dendl;
      return -AUTH_ERR;
    }
    else if (retval == PAM_USER_UNKNOWN)
    {
      generic_dout(10) << "User unknown." << dendl;
      return -USER_UNKNOWN;
    }
    else
    {
      generic_dout(10) << "pam_authenticate returned " << retval << dendl;
      return -OTHER_ERR;
    }
  }
  
  generic_dout(10) << "Authenticated." << dendl;
  
  retval = pam_end(local_auth_handle, retval);
  
  if (retval != PAM_SUCCESS)
  {
    generic_dout(10) << "pam_end returned " << retval << dendl;
    return OTHER_ERR;
  }
  
  return AUTH_OK;
}

int ClientUid::user_info(const char *username, int *uid, int *gid)
{
  struct passwd *ptr; 
  ptr = getpwnam(username);
  if (ptr == NULL )
    return USER_UNKNOWN;  
  
  generic_dout(10) << "User name " << ptr->pw_name << " uid " << ptr->pw_uid << dendl;
  *uid = ptr->pw_uid;
  *gid = ptr->pw_gid;
  
  return AUTH_OK; 
}
