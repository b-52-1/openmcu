
#include <ptlib.h>
#include <ptclib/guid.h>

#include "mcu.h"

////////////////////////////////////////////////////////////////////////////////////////////////////

int Registrar::OnReceivedMsg(msg_t *msg)
{
  PTRACE(1, "Registrar\tOnReceivedMessage");
  if(sep->terminating)
    return TRUE;

  sip_t *sip = sip_object(msg);
  if(sip->sip_cseq == NULL)
  {
    SipReqReply(msg, SIP_400_BAD_REQUEST);
    return TRUE;
  }

  PString username = sip->sip_from->a_url->url_user;
  PString username_out = sip->sip_to->a_url->url_user;

  int request = 0, status = 0, cseq = 0;
  if(sip->sip_request) request = sip->sip_request->rq_method;
  if(sip->sip_status)  status = sip->sip_status->st_status;
  if(sip->sip_cseq)    cseq = sip->sip_cseq->cs_method;

  // register
  if(request == sip_method_register)
  {
    OnReceivedSipRegister(msg);
    return TRUE;
  }
  // publish
  if(request == sip_method_publish)
  {
    SipReqReply(msg, SIP_200_OK);
    return TRUE;
  }
  // options
  if(request == sip_method_options)
  {
    SipReqReply(msg, SIP_200_OK);
    return TRUE;
  }
  //
  if(username == username_out)
  {
    if(request == sip_method_invite)
      SipReqReply(msg, SIP_486_BUSY_HERE);
    else
      SipReqReply(msg, SIP_200_OK);
    return TRUE;
  }

  // subscribe dialog, notify receive in callback function
  if(request == sip_method_subscribe)
  {
    OnReceivedSipSubscribe(msg);
    return TRUE;
  }
  // notify only receive in subscribe callback function
//  if(request == sip_method_notify)
//  {
//    SipReqReply(msg, SIP_481_NO_TRANSACTION);
//    return TRUE;
//  }

  // message dialog
  if(request == sip_method_message)
  {
    OnReceivedSipMessage(msg);
    return TRUE;
  }

  // invite
  if(request == sip_method_invite)
  {
    OnReceivedSipInvite(msg);
    return TRUE;
  }

  if(request == sip_method_refer ||
     request == sip_method_update)
  {
    SipReqReply(msg, SIP_501_NOT_IMPLEMENTED);
    return TRUE;
  }

  return FALSE;
// request == "REGISTER"
// request == "PUBLISH"
// request == "SUBSCRIBE"
// request == "NOTIFY"
// request == "UPDATE"
// request == "REFER"
// request == "MESSAGE"
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int Registrar::OnReceivedSipRegister(const msg_t *msg)
{
  PTRACE(1, "Registrar\tOnReceivedSipRegister");
  sip_t *sip = sip_object(msg);

  MCUURL_SIP url(msg, 0);
  PString username = url.GetUserName();

  RegistrarAccount *regAccount = FindAccountWithLock(ACCOUNT_TYPE_SIP, username);
  if(!regAccount && !sip_require_password)
  {
    regAccount = InsertAccountWithLock(ACCOUNT_TYPE_SIP, username);
  }

  int response_code = SipPolicyCheck(msg, regAccount, NULL);
  if(response_code != 1)
  {
    goto return_response;
  }

  {
    // update account data
    regAccount->host = url.GetHostName();
    regAccount->port = atoi(url.GetPort());
    regAccount->transport = url.GetSipProto();
    if(regAccount->display_name == "")
      regAccount->display_name = url.GetDisplayName();
    regAccount->remote_application = url.GetRemoteApplication();
    // TTL
    regAccount->registered = TRUE;
    regAccount->start_time = PTime();
    if(sip->sip_expires)
      regAccount->expires = sip->sip_expires->ex_delta;
    else
      regAccount->expires = 600;
    // store register message
    msg_destroy(regAccount->msg_reg);
    regAccount->msg_reg = msg_dup(msg);

    response_code = 200;
    goto return_response;
  }
  return_response:
    if(regAccount) regAccount->Unlock();
    if(response_code == 0)
      return 0;
    else
      return SipReqReply(msg, response_code, regAccount->GetAuthStr());
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int Registrar::OnReceivedSipMessage(msg_t *msg)
{
  PTRACE(1, "Registrar\tOnReceivedSipMessage");
  sip_t *sip = sip_object(msg);

  if(sip->sip_payload == NULL)
    return SipReqReply(msg, SIP_415_UNSUPPORTED_MEDIA);

  PString username_in = sip->sip_from->a_url->url_user;
  PString username_out = sip->sip_to->a_url->url_user;

  RegistrarAccount *regAccount_in = NULL;
  RegistrarAccount *regAccount_out = NULL;
  regAccount_in = FindAccountWithLock(ACCOUNT_TYPE_SIP, username_in);
  regAccount_out = FindAccountWithLock(ACCOUNT_TYPE_SIP, username_out);

  int response_code = SipPolicyCheck(msg, regAccount_in, regAccount_out);
  if(response_code != 1)
  {
    goto return_response;
  }

  {
    if(regAccount_out->account_type == ACCOUNT_TYPE_SIP)
      SipSendMessage(regAccount_in, regAccount_out, PString(sip->sip_payload->pl_data));
    response_code = 200;
  }
//  else if(regAccount_out->account_type == ACCOUNT_TYPE_H323)
//    H323SendMessage(regAccount_out, PString(sip->sip_payload->pl_data));

  return_response:
    if(regAccount_in) regAccount_in->Unlock();
    if(regAccount_out) regAccount_out->Unlock();
    if(response_code == 0)
      return 0;
    else
      return SipReqReply(msg, response_code, regAccount_in->GetAuthStr());
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int Registrar::OnReceivedSipInvite(const msg_t *msg)
{
  PTRACE(1, "Registrar\tOnReceivedSipInvite");
  sip_t *sip = sip_object(msg);

  PString callToken = "sip:"+PString(sip->sip_from->a_url->url_user)+":"+PString(sip->sip_call_id->i_id);
  if(FindRegConn(callToken))
    return 0;

  MCUURL_SIP url(msg, 0);
  PString username_in = url.GetUserName();
  PString username_out = sip->sip_to->a_url->url_user;

  RegistrarAccount *regAccount_in = NULL;
  RegistrarAccount *regAccount_out = NULL;
  RegistrarConnection *regConn = NULL;

  regAccount_in = FindAccountWithLock(ACCOUNT_TYPE_SIP, username_in);

  if(allow_internal_calls)
  {
    regAccount_out = FindAccountWithLock(ACCOUNT_TYPE_UNKNOWN, username_out);
  }

  if((!regAccount_in && sip_allow_unauth_mcu_calls && !regAccount_out) ||
     (!regAccount_in && sip_allow_unauth_internal_calls && regAccount_out))
  {
    regAccount_in = InsertAccountWithLock(ACCOUNT_TYPE_SIP, username_in);
  }

  int response_code = SipPolicyCheck(msg, regAccount_in, regAccount_out);
  if(response_code != 1)
  {
    goto return_response;
  }

  // update account data ???
  if(!regAccount_in->registered)
  {
    regAccount_in->host = url.GetHostName();
    regAccount_in->port = atoi(url.GetPort());
    regAccount_in->transport = url.GetSipProto();
    if(regAccount_in->display_name == "")
      regAccount_in->display_name = url.GetDisplayName();
    regAccount_in->remote_application = url.GetRemoteApplication();
  }

  {
    if(!regAccount_out) // MCU call if !regAccount_out
    {
      // create MCU sip connection
      MCUSipConnection *sCon = new MCUSipConnection(sep, ep, callToken);
      sCon->direction = 0;
      sCon->c_sip_msg = msg_dup(msg);
      // create registrar connection
      regConn = InsertRegConnWithLock(callToken, username_in, username_out);
      msg_destroy(regConn->msg_invite);
      regConn->msg_invite = msg_dup(msg);
      regConn->account_type_in = regAccount_in->account_type;
      regConn->state = CONN_MCU_WAIT;
      response_code = -1; // MCU call
      goto return_response;
    }
    else
    {
      if(regAccount_out->account_type == ACCOUNT_TYPE_SIP && regAccount_in->sip_call_processing != "full" && regAccount_out->sip_call_processing != "full")
      {
        SipReqReply(msg, 100);
        SipReqReply(msg, 180);
        SipReqReply(msg, 302, "", regAccount_out->GetUrl());
        response_code = 0; // EMPTY
        goto return_response;
      }
      else
      {
        // create MCU sip connection
        MCUSipConnection *sCon = new MCUSipConnection(sep, ep, callToken);
        sCon->direction = 0;
        sCon->c_sip_msg = msg_dup(msg);
        // create registrar connection
        regConn = InsertRegConnWithLock(callToken, username_in, username_out);
        msg_destroy(regConn->msg_invite);
        regConn->msg_invite = msg_dup(msg);
        regConn->roomname = MCU_INTERNAL_CALL_PREFIX + OpalGloballyUniqueID().AsString();
        regConn->state = CONN_WAIT;
        response_code = 100; // SIP_100_TRYING
        goto return_response;
      }
    }
  }

  return_response:
    if(regAccount_in) regAccount_in->Unlock();
    if(regAccount_out) regAccount_out->Unlock();
    if(regConn) regConn->Unlock();
    if(response_code == 0)
      return 0;
    else if(response_code == -1)
      sep->CreateIncomingConnection(msg); // MCU call
    else
      SipReqReply(msg, response_code, regAccount_in->GetAuthStr());
    return 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int Registrar::OnReceivedSipSubscribe(msg_t *msg)
{
  PTRACE(1, "Registrar\tOnReceivedSipSubscribe");
  sip_t *sip = sip_object(msg);
  if(!sip->sip_expires)
    return 0;

  if(!sip->sip_event || (sip->sip_event && PString(sip->sip_event->o_type) != "presence"))
    return 0;

  RegistrarAccount *regAccount_in = NULL;
  Subscription *subAccount = NULL;

  PString username_in = sip->sip_from->a_url->url_user;
  PString username_out = sip->sip_to->a_url->url_user;
  PString username_pair = username_in+"@"+username_out;

  regAccount_in = FindAccountWithLock(ACCOUNT_TYPE_SIP, username_in);

  int response_code = SipPolicyCheck(msg, regAccount_in, NULL);
  if(response_code != 1)
  {
    goto return_response;
  }

  {
    subAccount = FindSubWithLock(username_pair);
    if(subAccount)
      subAccount->Reset();
    else
      subAccount = InsertSubWithLock(this, username_in, username_out);

    if(sip->sip_expires)
      subAccount->expires = sip->sip_expires->ex_delta;
    else
      subAccount->expires = 600;
    // create to_tag for reply
    msg_header_add_param(msg_home(msg), (msg_common_t *)sip->sip_to, nta_agent_newtag(GetHome(), "tag=%s", GetAgent()));
    //
    msg_destroy(subAccount->msg_sub);
    subAccount->msg_sub = msg_dup(msg);
    //
    response_code = 202; // SIP_202_ACCEPTED
    goto return_response;
  }

  return_response:
    if(regAccount_in) regAccount_in->Unlock();
    if(subAccount) subAccount->Unlock();
    if(response_code == 0)
      return 0;
    else
      return SipReqReply(msg, response_code, regAccount_in->GetAuthStr());
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int Registrar::SipPolicyCheck(const msg_t *msg, RegistrarAccount *regAccount_in, RegistrarAccount *regAccount_out)
{
  PTRACE(1, "Registrar\tSipPolicyCheck");
  sip_t *sip = sip_object(msg);

  if(!sip->sip_request)
    return 1;

  if(!regAccount_in)
    return 403; // SIP_403_FORBIDDEN

  int request = sip->sip_request->rq_method;
  PString request_name = sip->sip_request->rq_method_name;

  if(sip->sip_authorization)
  {
    PString reg_response = msg_params_find(sip->sip_authorization->au_params, "response=");
    PString reg_uri = msg_params_find(sip->sip_authorization->au_params, "uri=");

    PString sip_auth_str = sep->MakeAuthStr(regAccount_in->username, regAccount_in->password, reg_uri, request_name, regAccount_in->scheme, regAccount_in->domain, regAccount_in->nonce);
    sip_authorization_t *sip_auth = sip_authorization_make(msg_home(msg), sip_auth_str);
    regAccount_in->www_response = msg_params_find(sip_auth->au_params, "response=");
    if(regAccount_in->www_response == reg_response)
      return 1;
  }
  else if(sip->sip_proxy_authorization)
  {
    PString reg_response = msg_params_find(sip->sip_proxy_authorization->au_params, "response=");
    PString reg_uri = msg_params_find(sip->sip_proxy_authorization->au_params, "uri=");

    PString sip_auth_str = sep->MakeAuthStr(regAccount_in->username, regAccount_in->password, reg_uri, request_name, regAccount_in->scheme, regAccount_in->domain, regAccount_in->nonce);
    sip_authorization_t *sip_auth = sip_authorization_make(msg_home(msg), sip_auth_str);
    regAccount_in->proxy_response = msg_params_find(sip_auth->au_params, "response=");
    if(regAccount_in->proxy_response == reg_response)
      return 1;
  }

  if(request == sip_method_register)
  {
    if(!regAccount_in->enable && sip_require_password)
      return 403; // SIP_403_FORBIDDEN
    if(!sip_require_password)
      return 1;
    if(regAccount_in->password == "")
      return 1;
    return 401; // SIP_401_UNAUTHORIZED
  }
  if(request == sip_method_invite)
  {
    if(regAccount_out && regAccount_out->host == "")
      return 404; // SIP_404_NOT_FOUND
    if(regAccount_out && sip_allow_unauth_internal_calls)
      return 1;
    if(!regAccount_out && sip_allow_unauth_mcu_calls)
      return 1;
    if(regAccount_in->password == "")
      return 1;
    return 407; // SIP_407_PROXY_AUTH_REQUIRED
  }
  if(request == sip_method_message)
  {
    if(!regAccount_out || (regAccount_out && !regAccount_out->registered) || (regAccount_out && regAccount_out->host == ""))
      return 404; // SIP_404_NOT_FOUND
    if(regAccount_in->password == "")
      return 1;
    return 407; // SIP_407_PROXY_AUTH_REQUIRED
  }
  if(request == sip_method_subscribe)
  {
    if(regAccount_in->password == "")
      return 1;
    return 407; // SIP_407_PROXY_AUTH_REQUIRED
  }
  if(request == sip_method_cancel || request == sip_method_ack)
  {
    return 1;
  }
  return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int Registrar::SipSendNotify(msg_t *msg_sub, int state)
{
  PTRACE(1, "MCUSIP\tSipSendNotify");
  sip_t *sip_sub = sip_object(msg_sub);
  if(sip_sub == NULL) return 0;

  PString basic;
  if(state == SUB_STATE_OPEN)
    basic = "open";
  else
    basic = "closed";

  PString state_rpid; // http://tools.ietf.org/search/rfc4480
  if(state == SUB_STATE_BUSY)
    state_rpid = "on-the-phone";

  PString sip_contact_str = "sip:"+PString(sip_sub->sip_to->a_url->url_user)+"@"+PString(sip_sub->sip_to->a_url->url_host);

  PString sip_payload_str = "<?xml version='1.0' encoding='UTF-8'?>"
      "<presence xmlns='urn:ietf:params:xml:ns:pidf'"
//      " xmlns:ep='urn:ietf:params:xml:ns:pidf:status:rpid-status'"
//      " xmlns:et='urn:ietf:params:xml:ns:pidf:rpid-tuple'"
//      " xmlns:ci='urn:ietf:params:xml:ns:pidf:cipid'"
      " xmlns:dm='urn:ietf:params:xml:ns:pidf:data-model'"
      " xmlns:rpid='urn:ietf:params:xml:ns:pidf:rpid'"
      " entity='"+sip_contact_str+"'>"
      "<tuple id='sg89ae'>"
        "<status>"
          "<basic>"+basic+"</basic>"
//          "<st:state>"+state+"</st:state>"
//          "<ep:activities><ep:activity>"+state+"</ep:activity></ep:activities>"
        "</status>"
      "</tuple>";
//      "<ci:display-name></ci:display-name>"

  if(state_rpid != "")
    sip_payload_str +=
      "<dm:person id='sg89aep'>"
        "<rpid:activities><rpid:"+state_rpid+"/></rpid:activities>"
//        "<dm:note>Idle</dm:note>"
      "</dm:person>";

  sip_payload_str += "</presence>";

  url_string_t *ruri = (url_string_t *)(const char *)MCUURL_SIP(msg_sub, 0).GetUrl();;
  sip_contact_t *sip_contact = sip_contact_create(GetHome(), (url_string_t *)(const char *)sip_contact_str, NULL);

  // cseq increment for incoming sub request
  sip_cseq_t *sip_cseq = sip_cseq_create(GetHome(), sip_sub->sip_cseq->cs_seq+1, SIP_METHOD_NOTIFY);
  msg_header_insert(msg_sub, (msg_pub_t *)sip_sub, (msg_header_t *)sip_cseq);

  sip_request_t *sip_rq = sip_request_create(GetHome(), SIP_METHOD_NOTIFY, ruri, NULL);
  sip_route_t* sip_route = sip_route_reverse(GetHome(), sip_sub->sip_record_route);

  msg_t *msg_req = nta_msg_create(GetAgent(), 0);
  nta_outgoing_mcreate(GetAgent(), NULL, NULL,
			ruri,
			msg_req,
			NTATAG_STATELESS(1),
			SIPTAG_FROM(sip_sub->sip_to),
			SIPTAG_TO(sip_sub->sip_from),
			SIPTAG_CONTACT(sip_contact),
			SIPTAG_ROUTE(sip_route),
 			SIPTAG_REQUEST(sip_rq),
			SIPTAG_CSEQ(sip_cseq),
			SIPTAG_CALL_ID(sip_sub->sip_call_id),
			SIPTAG_EVENT_STR("presence"),
                        SIPTAG_CONTENT_TYPE_STR("application/pidf+xml"),
                        SIPTAG_PAYLOAD_STR(sip_payload_str),
                        SIPTAG_SUBSCRIPTION_STATE_STR("active"), // active;expires=xxx
			SIPTAG_SERVER_STR(MCUSIP_USER_AGENT_STR),
			TAG_END());
  return 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int Registrar::SipSendMessage(RegistrarAccount *regAccount_in, RegistrarAccount *regAccount_out, PString message)
{
  PTRACE(1, "MCUSIP\tServerSendMessage");
  sip_t *sip_reg_out = sip_object(regAccount_out->msg_reg);

  url_string_t *ruri = (url_string_t *)(const char *)regAccount_out->GetUrl();

  PString url_from = "sip:"+regAccount_in->username+"@"+regAccount_out->domain;
  sip_addr_t *sip_from = sip_from_create(GetHome(), (url_string_t *)(const char *)url_from);
  sip_from_tag(GetHome(), sip_from, nta_agent_newtag(GetHome(), "tag=%s", GetAgent()));

  PString url_to = "sip:"+regAccount_out->username+"@"+regAccount_out->domain;
  sip_addr_t *sip_to = sip_to_create(GetHome(), (url_string_t *)(const char *)url_to);

  sip_cseq_t *sip_cseq = sip_cseq_create(GetHome(), 1, SIP_METHOD_MESSAGE);
  sip_request_t *sip_rq = sip_request_create(GetHome(), SIP_METHOD_MESSAGE, ruri, NULL);
  sip_call_id_t* sip_call_id = sip_call_id_create(GetHome(), "");

  sip_route_t* sip_route = NULL;
  if(sip_reg_out)
    sip_route = sip_route_reverse(GetHome(), sip_reg_out->sip_record_route);

  msg_t *msg_req = nta_msg_create(GetAgent(), 0);
  nta_outgoing_mcreate(GetAgent(), NULL, NULL,
			ruri,
			msg_req,
			NTATAG_STATELESS(1),
			SIPTAG_FROM(sip_from),
			SIPTAG_TO(sip_to),
			//SIPTAG_CONTACT(sip_contact),
			SIPTAG_ROUTE(sip_route),
 			SIPTAG_REQUEST(sip_rq),
			SIPTAG_CSEQ(sip_cseq),
			SIPTAG_CALL_ID(sip_call_id),
			SIPTAG_CONTENT_TYPE_STR("text/plain"),
                        SIPTAG_PAYLOAD_STR(message),
			SIPTAG_SERVER_STR(MCUSIP_USER_AGENT_STR),
			TAG_END());
  return 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int Registrar::SipReqReply(const msg_t *msg, unsigned method, PString auth_str, PString contact)
{
  //PTRACE(1, "Registrar\tSipReqReply");
  sip_t *sip = sip_object(msg);
  if(!sip || !sip->sip_request)
    return 0;

  const char *method_name = sip_status_phrase(method);
  if(method_name == NULL)
    return 0;

  sip_authorization_t *sip_www_auth=NULL, *sip_proxy_auth=NULL;
  if(auth_str != "")
  {
    if(method == 401)
      sip_www_auth = sip_authorization_make(sep->GetHome(), auth_str);
    else if(method == 407)
      sip_proxy_auth = sip_authorization_make(sep->GetHome(), auth_str);
  }

  sip_contact_t *sip_contact = NULL;
  if(contact != "")
  {
    sip_contact = sip_contact_create(sep->GetHome(), (url_string_t *)(const char *)contact, NULL);
  }

  const char *event = NULL;
  const char *allow_events = NULL;
  if(sip->sip_request->rq_method == sip_method_register)
  {
    event = "registration";
    allow_events = "presence";
    if(!sip_contact) sip_contact = sip->sip_contact;
  }
  PString allow = "SUBSCRIBE, INVITE, ACK, BYE, CANCEL, OPTIONS, INFO";

  sip_expires_t *sip_expires = NULL;
  if(sip->sip_request->rq_method == sip_method_register || sip->sip_request->rq_method == sip_method_subscribe)
  {
    sip_expires = sip->sip_expires;
  }

  msg_t *msg_reply = msg_dup(msg);
  nta_msg_treply(sep->GetAgent(), msg_reply, method, method_name,
                   SIPTAG_CONTACT(sip_contact),
		   SIPTAG_EXPIRES(sip_expires),
  		   SIPTAG_WWW_AUTHENTICATE(sip_www_auth),
		   SIPTAG_PROXY_AUTHENTICATE(sip_proxy_auth),
  		   SIPTAG_EVENT_STR(event),
  		   SIPTAG_ALLOW_EVENTS_STR(allow_events),
  		   SIPTAG_ALLOW_STR(allow),
                   //SIPTAG_CONTENT_TYPE_STR(content_str),
                   //SIPTAG_PAYLOAD(sip_payload),
                   SIPTAG_SERVER_STR((const char*)(MCUSIP_USER_AGENT_STR)),
                   TAG_END());
  return 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
