#include <glib/gstdio.h>
#define __USE_GNU 1
#include "prdr-mod.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include <src/modules/mod_sieve/message.h>

extern GKeyFile *prdr_inifile;
extern char *prdr_section;
static gboolean mod_sieve_vacation;
static int mod_sieve_vacation_days_min = 1;
static int mod_sieve_vacation_days_default = 7;
static int mod_sieve_vacation_days_max = 30;
static gboolean mod_sieve_notify_mailto;// = "true";
static gboolean mod_sieve_notify_sip;// = "true";
static gboolean mod_sieve_notify_xmpp;// = "true";
static gboolean mod_sieve_redirect;// = "true";
int libsieve_run (void *);

struct script {
  char* name;
  char* script;
};

struct sieve_global {
  GHashTable* hashTable;
  GHashTable* recipients; //default scripts for the recipients
};

static void
free_action_list(action_list_t *a)
{
  action_list_t *b;
  while (a) {
    switch (a->a) {
    case ACTION_REDIRECT:
      if (a->u.red.addr) g_free(a->u.red.addr);
      break;
    case ACTION_REJECT:
      if (a->u.rej.msg) g_free(a->u.rej.msg);
      break;
    case ACTION_VACATION:
      if(a->u.vac.send.addr) g_free(a->u.vac.send.addr);
      if(a->u.vac.send.fromaddr) g_free(a->u.vac.send.fromaddr);
      if(a->u.vac.send.msg) g_free(a->u.vac.send.msg);
      if(a->u.vac.send.subj) g_free(a->u.vac.send.subj);
      if(a->u.vac.autoresp.hash) g_free(a->u.vac.autoresp.hash);
      break;
    case ACTION_NULL:
    case ACTION_NONE:
    case ACTION_FILEINTO:
    case ACTION_DISCARD:
    case ACTION_NOTIFY:
    case ACTION_KEEP: break;
    }
    b = a->next;
    g_free(a);
    a = b;
  }
}

static inline char*
substitute_named_variable (struct privdata *cont,
			   char* variable)
{
  if (strcmp (variable, "envelope.from") == 0)
      return g_strdup (prdr_get_envsender(cont));
  if (strcmp (variable, "envelope.to") == 0)
      return g_strdup (prdr_get_recipient(cont));
  if (strstr(variable, "headers.") == variable) {
      char **headers;
      if (prdr_get_stage (cont) < MOD_HEADERS ) {
          prdr_do_fail (cont);
	  struct sieve_local *dat =
	    (struct sieve_local*)prdr_get_priv_rcpt (cont);
	  dat->desired_stages |= MOD_HEADERS;
	  prdr_do_fail (cont);
	  return g_strdup ("defect");
      }
      headers = prdr_get_header (cont, variable + 8);
      if (headers) {
	char *ret = g_strdup (headers[0]);
	g_free (headers);
	return ret;
      } else return g_strdup ("nichts gefunden");
  }
  if (strcmp (variable, "advertisement") == 0)
    return g_strdup ("");//----    ----    ----    ----    ----\r\nClick to get your own mailing list:\r\nhttps://lists.aegee.org/new\r\n    Service provided by AEGEE Mail Team\r\n    http://mail.aegee.org/\r\n----    ----    ----    ----    ----");
  return g_strdup ("");
}

static inline char*
expand_variables_in_string (struct privdata *cont,
			    const char * const string)
{
  int len = strlen (string), s1 = 0, s2 = 0, j;
  char* string2 = g_malloc (len+1);
  //string - original string
  //string2 - new string
  //j + s1 - position in the first string
  //j + s2 - position in the second string
  for (j = 0; j + s1 < len; j++)
    if (string[j + s1] == '$' && string[j+1 + s1] == '{') {
      int k = j + 2 + s1;
      while (string[k] != '{' && string[k] != '}' &&
             string[k] != '$' && string[k] != '\0')
	k++;
      if (string[k] == '}') {
	char *var_name = g_malloc (k- j - s1 - 1);
        strncpy (var_name, string+j + s1 + 2, k-j- s1 - 2);
        var_name[k-j-s1-2] = '\0';
	char *var_value = substitute_named_variable (cont, var_name);
	g_free (var_name);
	int var_value_length = strlen (var_value);
	string2[j + s2] = '\0';
	s2 += var_value_length - 1;
	s1 += k - j - s1;
        string2 = realloc (string2, len + s2 - s1 + 1);
	strcat (string2, var_value);
	g_free(var_value);
      } else
	string2[j + s2] = string[j + s1];
    } else
      string2[j + s2] = string[j + s1];
  string2[j + s2] = '\0';
  return string2;
}

#define test_var_subst(x) y = expand_variables_in_string (NULL, x); g_printf ("%s->%s--\n", x, y); free (y);

int mod_sieve_LTX_load ()
{
  mod_sieve_notify_mailto = TRUE;
  mod_sieve_notify_sip = TRUE;
  mod_sieve_notify_xmpp = TRUE;
  mod_sieve_redirect = TRUE;
  int vac_err = 0;
  char **array = g_key_file_get_keys (prdr_inifile, prdr_section, NULL, NULL);
  if (array) {
    int i = 0;
    while (array[i]) {
      if (strcmp (array[i], "vacation") == 0) {
	mod_sieve_vacation = g_key_file_get_boolean (prdr_inifile,
						     prdr_section, "vacation",
						     NULL); } else
      if (strcmp(array[i], "vacation_days_min") == 0) {
	mod_sieve_vacation_days_min = g_key_file_get_integer (prdr_inifile,
							      prdr_section,
							      "vacation_days_min",
							      NULL); } else
      if (strcmp(array[i], "vacation_days_default") == 0) {
	mod_sieve_vacation_days_default = g_key_file_get_integer (prdr_inifile,
								  prdr_section,
								  "vacation_days_default",
								  NULL); } else
      if (strcmp(array[i], "vacation_days_max") == 0) {
	mod_sieve_vacation_days_max = g_key_file_get_integer (prdr_inifile, prdr_section, "vacation_days_max", NULL); } else
      if (strcmp(array[i], "notify_mailto") == 0) {
        mod_sieve_notify_mailto = g_key_file_get_boolean (prdr_inifile,
							  prdr_section,
							  "notify_mailto",
							  NULL); } else
      if (strcmp (array[i], "notify_sip") == 0) {
	mod_sieve_notify_sip = g_key_file_get_boolean (prdr_inifile,
						       prdr_section,
						       "notify_sip",
						       NULL); } else
      if (strcmp (array[i], "notify_xmpp") == 0) {
	mod_sieve_notify_xmpp = g_key_file_get_boolean (prdr_inifile,
							prdr_section,
							"notify_xmpp",
							NULL); } else
      if (strcmp (array[i], "redirect") == 0) {
	mod_sieve_redirect = g_key_file_get_boolean (prdr_inifile,
						     prdr_section,
						     "redirect", NULL); } else {
	  g_printf ("option %s in section [%s] is not recognized\n", array[i], prdr_section);
	  g_strfreev (array);
	  return -1;
      }
      i++;
    }
    g_strfreev (array);
  };
  if (mod_sieve_vacation_days_min < 1) {
    vac_err = 1;
    g_printf ("vacation_days_min is set to %i in section %s, but it cannot be less than one\n", mod_sieve_vacation_days_min, prdr_section);
  }
  if (mod_sieve_vacation_days_max < 1) {
    vac_err = 1;
    g_printf ("vacation_days_max is set to %i in section %s, but it cannot be less than one\n", mod_sieve_vacation_days_max, prdr_section);
  }
  if (mod_sieve_vacation_days_min > mod_sieve_vacation_days_default) {
    vac_err = 1;
    g_printf ("vacation_days_default (%i) cannot be set to less than vacation_days_min (%i) in section %s\n", mod_sieve_vacation_days_default, mod_sieve_vacation_days_min, prdr_section);
  }
  if (mod_sieve_vacation_days_default > mod_sieve_vacation_days_max) {
    vac_err = 1;
    g_printf ("vacation_days_default(%i) cannot be greater than vacation_days_max(%i) in section %s\n", mod_sieve_vacation_days_default, mod_sieve_vacation_days_max, prdr_section);
  }
  if (vac_err) {
    g_printf ("--> vacation is disabled\n");
    mod_sieve_vacation = FALSE;
  }
  return 0;
}

int
mod_sieve_LTX_prdr_mod_destroy_msg (void* private)
{
  struct privdata *cont = (struct privdata*) private;
  struct sieve_global *glob = (struct sieve_global*)prdr_get_priv_msg (cont);
  g_hash_table_destroy (glob->recipients);
  g_hash_table_destroy (glob->hashTable);
  g_free (glob);
  return 0;
}

int
mod_sieve_LTX_prdr_mod_status (void* priv)
{
  if (prdr_get_stage (priv) & (MOD_MAIL | MOD_EHLO))
    return MOD_RCPT;
  else {
    struct sieve_local* dat = (struct sieve_local*) prdr_get_priv_rcpt (priv);
    return dat->desired_stages;
  }
}

//rcpt == NULL || rcpt == "" -> global default script
static char*
get_default_script_for_recipient (void* my, char* rcpt)
{
  struct privdata *cont = (struct privdata*) my;
  struct sieve_global *glob = (struct sieve_global*) prdr_get_priv_msg (cont);
  if (rcpt == NULL) rcpt = "";
  char *temp;
  if ( !g_hash_table_lookup_extended (glob->recipients,
				      rcpt, NULL, (gpointer*)&temp) ) {
    temp = prdr_list_query ("sieve_scripts",
			    (*rcpt == '\0') ? ":global" : rcpt, "");
    g_hash_table_insert (glob->recipients, rcpt, temp);
  }
  /* //for the future
   * if (temp && (strstr (temp, "vacation") != NULL
   *	       || strstr (temp, "X-Aegee-Spam-Level") != NULL)) {
   *    prdr_set_activity (cont, "spamassassin", 1);
   *  //g_printf ("spamassassin activated for %s\n", prdr_get_recipient(cont));
   *  } else
   *    prdr_set_activity (cont, "spamassassin", 2);
   */
  return temp;
}

char *
sieve_getscript (char const *const path, char const * const name, void *my)
{
  struct privdata *cont = (struct privdata*) my;
  char *temp;
  if ( *path == '\0' || path == NULL || strcmp (path, ":global")) {//the scope is private
    if (*name == '\0' || name == NULL) {
      //default script for recipient
      char *def = get_default_script_for_recipient (my, cont->current_recipient->address);
      return def == NULL ?
	/* load the global default script */ get_default_script_for_recipient (my, "") : def;
    }
    struct sieve_local *dat = (struct sieve_local*) prdr_get_priv_rcpt (cont);
    if (!g_hash_table_lookup_extended (dat->hashTable, name, NULL,
				       (gpointer*)&temp)) {
      temp = prdr_list_query ("sieve_scripts", prdr_get_recipient (cont),
			      name);
      g_hash_table_insert (dat->hashTable, (char*)name, temp);
    }
  } else { //the scope is global
    if (*name == '\0' || name == NULL) {
      return get_default_script_for_recipient (my, "");
    }
    struct sieve_global *glob =
      (struct sieve_global*) prdr_get_priv_msg (cont);
    if (!g_hash_table_lookup_extended (glob->hashTable, name, NULL,
				       (gpointer*)&temp)) {
      temp = prdr_list_query ("sieve_scripts", ":global", name);
      g_hash_table_insert (glob->hashTable, (char*)name, temp);
    }
  }
  return temp;
}

//returns 1 if the recipients are considered equal
int
mod_sieve_LTX_prdr_mod_equal (struct privdata* priv, char* user1, char* user2)
{
  char *u1 = get_default_script_for_recipient (priv, user1);
  char *u2 = get_default_script_for_recipient (priv, user2);
  if (u1 == NULL && u2 == NULL) return 1;
  if (u1 != NULL && u2 != NULL) return (0 == g_ascii_strcasecmp(u1, u2));
  return 0;
}

int
mod_sieve_LTX_prdr_mod_init_rcpt (void* private)
{
  struct privdata *cont = (struct privdata*) private;
  struct sieve_local *dat = g_malloc0 (sizeof (struct sieve_local));
  dat->redirect_to = g_ptr_array_new_with_free_func (g_free);
  dat->desired_stages = MOD_RCPT;
  dat->hashTable   = g_hash_table_new_full (g_str_hash, g_str_equal,
					    NULL, g_free);
  prdr_set_priv_rcpt (cont, dat);
  return 0;
}

int
mod_sieve_LTX_prdr_mod_init_msg (void* private)
{
  struct privdata *cont = (struct privdata*) private;
  struct sieve_global *glob = g_malloc (sizeof (struct sieve_global));
  glob->hashTable  = g_hash_table_new_full (g_str_hash, g_str_equal,
					    NULL, g_free);
  glob->recipients = g_hash_table_new_full (g_str_hash, g_str_equal,
					    NULL, g_free);
  prdr_set_priv_msg (cont, glob);
  return 0;
}

int
mod_sieve_LTX_prdr_mod_destroy_rcpt (void* private)
{
  struct privdata *cont = (struct privdata*) private;
  struct sieve_local* dat = (struct sieve_local*)prdr_get_priv_rcpt (cont);
  g_hash_table_destroy (dat->hashTable);
  g_ptr_array_free (dat->redirect_to, TRUE);
  if (dat->headers) {
    g_free (dat->headers);
    dat->headers = NULL;
  }
  g_free (dat);
  return 0;
}

/*
int
sieve_fileinto (sieve2_context_t *s, void *my)
{
  struct privdata *cont = (struct privdata*) my;
//start comment
  struct sieve_local *dat = (struct sieve_local*)prdr_get_priv_rcpt(cont);
  if (prdr_get_stage(cont) != MOD_BODY) {
    dat->desired_stages |= MOD_BODY;
    prdr_do_fail(cont);
    return SIEVE2_ERROR_FAIL;
  };
  char *fileinto = g_strconcat(prdr_get_recipient(cont), "+", sieve2_getvalue_string(s, "mailbox"), NULL);
  prdr_add_recipient(cont, prdr_add_string(priv, fileinto));
  g_free(fileinto);
  prdr_del_recipient(cont, prdr_get_recipient(cont));
//end comment
  return SIEVE2_OK;
}
*/
static int
sieve_redirect (sieve_redirect_context_t context, void *my)
{
  struct privdata *cont = (struct privdata*) my;
  struct sieve_local *dat = (struct sieve_local*)prdr_get_priv_rcpt (cont);
  g_ptr_array_add ( dat->redirect_to, context.addr);
  GString *body = g_string_new ("Received: from ");
  char IP[256];
  switch (cont->hostaddr->sa_family) {
      case AF_INET:
	inet_ntop (AF_INET, &((struct sockaddr_in*)(cont->hostaddr))->sin_addr, IP, 256);
	break;
      case AF_INET6:
	inet_ntop (AF_INET6, &((struct sockaddr_in6*)(cont->hostaddr))->sin6_addr, IP, 256);
	break;
  }
  char* date = get_date ();
  g_string_append_printf (body, "%s [%s]\r\n        by %s id %s;\r\n        %s",
			  cont->ehlohost, IP, cont->domain_name,
			  cont->queue_id, date);
  g_free (date);
  g_string_append_printf (body, "\r\nResent-From: %s\r\nResent-To: ",
			  prdr_get_envsender (cont));
  char *temp = NULL;
  /*
  if (sieve2_getvalue_int (s, "list")) {
    char *list_plugin = g_strdup context.addr);
    temp = list_plugin;
    while (temp[0] && temp[0] != ':') temp++;
    if (temp[0] == ':') temp[0] = '\0';
    temp++;
    char *listname_keyword = temp;
    temp = prdr_list_query (list_plugin ,// "listserv" ,
			    listname_keyword, // listserv_list|keyword
			    NULL);
    prdr_list_insert ("log", prdr_get_recipient (cont),
		      "mod_sieve, action redirect, to: ", temp, 0);
    free (list_plugin);
    g_string_append_printf (body, "%s\r\n", temp);
    int i = 0;
    char *iter = temp;
    while (iter = strchr (iter, ',')) {
      i++;
      iter++;
    }
    rcpt = g_malloc (sizeof (char*) * (i+2));
    rcpt[i+1] = NULL;
    iter = temp;
    for (; i >= 0; i--) {
      rcpt[i] = iter;//FIXME presented addresses shall not be added twice
      //an address can come more than once in case of circular dependancies
      //List T1-L: Owner = Onwer(T2-L)   and
      //List T2-L: Owner = Owner(T1-L)
      iter = strchr (iter, ',');
      if (iter) {
	iter[0]= '\0';
	iter++;
      }
    };
  } else */ //no list, just @-like address
  char const * const rcpt[] = {context.addr, NULL};
  prdr_list_insert ("log", prdr_get_recipient (cont), "mod_sieve, action redirect, to: ", rcpt[0], 0);
  g_string_append_printf (body, "%s\r\n", rcpt[0]);
  //Resent-Sender, Resent-CC, Resent-BCC, Resent-Msg-Id
  GPtrArray* p = prdr_get_headers (cont);
  unsigned int i;
  struct header* h;
  //insert normal headers
  for (i = 0; i < p->len; i++) {
    h = g_ptr_array_index (p, i);
    g_string_append_printf (body, "%s: %s\r\n", h->field, h->value);
  }
  const GString *orig_body = prdr_get_body (cont);
  //prepend Received: header, as of RFC5231, Section 4.4. Trace Information
  g_string_append_printf (body, "\r\n%s", orig_body->str);
  //insert body
  prdr_sendmail ( strcmp (prdr_get_envsender(cont), "<>")
		  ? prdr_get_recipient(cont) : NULL, rcpt, body->str,
		  "Resent-Date", "auto-generated");
  g_string_free (body, 1);
  if (temp) g_free (temp);
  prdr_del_recipient (cont, prdr_get_recipient(cont));
  i = 0;
  return SIEVE2_OK;
}

static int
sieve_discard (void *my)
{
  struct privdata *cont = (struct privdata*) my;
  prdr_list_insert ("log", prdr_get_recipient (cont),
		    "mod_sieve, action discard, from: ",
		    prdr_get_envsender (cont), 0);
  prdr_del_recipient (cont, prdr_get_recipient (cont));
  return SIEVE2_OK;
}

static int
sieve_reject (struct sieve_reject_context context, void *my)
{
  struct privdata *cont = (struct privdata*) my;
  if (cont->current_recipient->current_module->flags & MOD_FAILED)
    return SIEVE2_ERROR_FAIL;
  prdr_set_response (cont, "550", "5.7.1",
		     expand_variables_in_string(cont, context.msg));
  prdr_del_recipient (cont, prdr_get_recipient (cont));
  char *text = g_strconcat ("from " , prdr_get_envsender (cont),
			    ", mod_sieve, action reject:", NULL);
  if ( (cont->current_recipient->current_module->flags & MOD_FAILED) == 0)
    prdr_list_insert ("log", prdr_get_recipient (cont), text, context.msg, 0);
  g_free (text);
  return SIEVE2_OK;
}
/*
static int
sieve_notify (sieve2_context_t *s, void *my)
{
  struct privdata *cont = (struct privdata*) my;
  if (prdr_get_stage (cont) != MOD_BODY) {
    struct sieve_local *dat = (struct sieve_local*)prdr_get_priv_rcpt (cont);
    dat->desired_stages |= MOD_BODY;
    prdr_do_fail (cont);
    return SIEVE2_ERROR_FAIL;
  }
  //active - otpadnalo
  //from - lipswa
  //id  - otpadnalo
  //importance - lipswa
  //priority - otpadnalo
  char const * const method = sieve2_getvalue_string (s, "method");
  //  char** options = sieve2_getvalue_stringlist (s, "options");
  char const * const message = sieve2_getvalue_string (s, "message");
  switch (*method) {
  case 's': //sip
    if (mod_sieve_notify_sip == FALSE)
      return SIEVE2_OK;
    break;
  case 'm': //mailto
    if (mod_sieve_notify_mailto == FALSE)
      return SIEVE2_OK;
    char **headers = prdr_get_header (cont, "Auto-Submitted");
    if (headers && headers[0] && (g_ascii_strcasecmp(headers[0], "No") != 0)) {
      g_free (headers);
      return SIEVE2_OK;
    }
    if (headers) g_free(headers);
    if (strstr(method, "mailto:") == method) {
      GString *body = g_string_new ("From: AEGEE Mail Team <mail@aegee.org>\r\n");
      char const * const recipient = method + 7;
      char *temp = strchr(method + 7, '?');
      if (temp) {
	temp[0] = '\0';
	temp++;
      }
      while (temp) {
	char *hname = temp;
	char *hvalue = strchr(temp, '=');
	if (g_ascii_strcasecmp (temp, "body") && hvalue)
	  g_string_append_printf (body, "%s: %s\r\n", hname, hvalue);
	temp = strchr (hvalue, '&');
      }
      g_string_append_printf (body, "To: %s\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\n%s", recipient, message);
      //log
      char const * const rcpt[]  = {recipient, NULL};
      char* auto_submitted = g_alloca (strlen (prdr_get_recipient(cont)) + 30);
      g_sprintf (auto_submitted, "auto-notified; owner-email=\"%s\"",
		 prdr_get_recipient(cont));
      prdr_sendmail ("mail@aegee.org", rcpt, body->str,
		     "Date", auto_submitted);
      g_string_free (body, TRUE);
      g_printf ("notifying mailto...\n");
    }
    break;
  case 'x': //xmpp
    if (mod_sieve_notify_xmpp == FALSE)
      return SIEVE2_OK;
    break;
  case 'z': //zephyr
    break;
  default:
    break;
  }
  return SIEVE2_OK;
}
*/

static int
sieve_vacation (struct sieve_autorespond_context autoresp,
		struct sieve_send_response_context send, void *my)
{
  if (mod_sieve_vacation == FALSE) return SIEVE2_OK;
  struct privdata *cont = (struct privdata*) my;
  char *temp = g_strdup (prdr_get_envsender (cont)), *temp1 = strchr (temp, '@');
  if (temp1 == NULL) return SIEVE2_OK;//this is a bounce -- no vacation
  temp1[0] = '\0';//begin section 4.6. of RFC 5230
  if (strstr (temp, "owner-") == temp ||
      strcasestr (temp, "-request") ||
      strcmp (temp, "<>") == 0 ||
      strcasestr (temp, "LISTSERV") ||
      strcasestr (temp, "MAILER-DAEMON") ||
      strcasestr (temp, "majordomo") ) {
    g_free (temp);
    return SIEVE2_OK;
  }
  g_free (temp);
  char **headers;
  if (prdr_get_stage (cont) >= MOD_HEADERS) {
    /*
    headers = prdr_get_header(cont, "X-Spam-Status");
    if (!headers) {
      prdr_add_header(cont, 0, "X-AEGEE-milter", "mod_sieve vacation not sent, as X-Spam-Status is missing");
      prdr_list_insert("log", prdr_get_recipient(cont), "vacation", "not sent -- X-Spam-Status is missing", 0);
      return SIEVE2_OK;
    }
    g_free(headers);
    */
    headers = prdr_get_header (cont, "X-Aegee-Spam-Level");
    if (headers && strlen (headers[0]) > 2) {
      g_free(headers);
      prdr_add_header (cont, 0, "X-AEGEE-milter",
		       "mod_sieve vacation not sent -- spam probability too high");
      prdr_list_insert ("log", prdr_get_recipient (cont), "vacation",
			"not sent -- spam probability is too high", 0);
      return SIEVE2_OK;
    }
    if (headers) g_free (headers);
  }
  if (autoresp.days == 0) autoresp.days = mod_sieve_vacation_days_default; else
  if (autoresp.days < mod_sieve_vacation_days_min) autoresp.days = mod_sieve_vacation_days_min; else
  if (autoresp.days > mod_sieve_vacation_days_max) autoresp.days = mod_sieve_vacation_days_max;

  char *handle = g_strconcat (prdr_get_envsender (cont), ":",
			      autoresp.hash, NULL);
  if (-1 == prdr_list_insert ("vacation", prdr_get_recipient (cont),
			      handle, "handle", 24 * 60 * 60 * autoresp.days)) {
    prdr_add_header (cont, 0, "X-AEGEE-milter", "mod_sieve vacation not sent -- already replied to this sender");
    prdr_list_insert ("log", prdr_get_recipient (cont), "vacation", "not sent -- already replied to this sender", 0);
    g_free (handle);
    return SIEVE2_OK;
  }
  g_free (handle);

  prdr_add_header (cont, 0, "X-AEGEE-milter", "mod_sieve vacation sent");
  prdr_list_insert ("log", prdr_get_recipient (cont), "vacation", "sent", 0);
  char const * const address UNUSED = send.addr;

  GString* msg = g_string_new ("Subject: ");
  g_string_append_printf (msg, "%s\r\n", send.subj);
  headers = prdr_get_header (cont, "References");
  g_string_append_printf (msg, "References: \r\n");
  if (headers) {
    int i = 0;
    while (headers[i])
      g_string_append_printf (msg, "  %s\r\n", headers[i++]);
    g_free(headers);
  } else {
    headers = prdr_get_header (cont, "In-Reply-To");
    if (headers) {
      g_string_append_printf (msg, "  %s\r\n", headers[0]);
      g_free (headers);
    }
  }
  headers = prdr_get_header (cont, "Message-Id");
  if (headers) {
    g_string_append_printf (msg, "  %s\r\n", headers[0]);//finish References
    g_string_append_printf (msg, "In-Reply-To: %s\r\n", headers[0]);
    g_free (headers);
  }
  g_string_append_printf (msg, "From: %s\r\nTo: %s\r\n",
			  send.fromaddr ? send.fromaddr : prdr_get_recipient(cont),
			  prdr_get_envsender (cont));
  //FIXME add RCPT
  if (send.mime)
    g_string_append_printf (msg, "Mime-Version: 1.0\r\n");
  else
    g_string_append (msg, "\r\n");
  g_string_append_printf (msg, send.msg);
  prdr_sendmail (NULL,
		 (const char const *[]){ prdr_get_envsender (cont) , NULL},
		 msg->str, "Date", "auto-replied");
  g_string_free (msg, 1);
  return SIEVE2_OK;
}

static int
sieve_keep (void *my)
{
  struct privdata *cont = (struct privdata*) my;
  prdr_add_recipient (cont, prdr_get_recipient (cont));
  return SIEVE2_OK;
}

/*
int
sieve_extlists (sieve2_context_t *s, void *my)
{
  char *list_module, *list_name = sieve2_getvalue_string (s, "list");
  struct privdata *cont = (struct privdata*) my;
  //  prdr_list_insert("log", prdr_get_recipient(cont), "sieve_extlists", "sent", 0);
  if (strchr (list_name, ':')) {
    list_module = g_strdup (list_name);
    char *x = strchr (list_module, ':');
    *x = '\0';
    list_name = x+1;
  };
  char *ret = prdr_list_query (list_module,
			       list_name,
			       sieve2_getvalue_string (s, "string"));
  char *temp = ret ? prdr_add_string (my, ret) : NULL;
  if (ret) g_free (ret);
  g_free (list_module);
  sieve2_setvalue_string (s, "result", temp);
  return 0;
}
*/

int
mod_sieve_LTX_prdr_mod_run (void *priv) {
//    prdr_add_header(priv, 0, "X-AEGEE-milter-mod_sieve", "sieve script executed");
  struct sieve_local* dat = (struct sieve_local*) prdr_get_priv_rcpt (priv);
  dat->actions = g_malloc(sizeof(action_list_t));
  if (dat->actions != NULL) {
	dat->actions->a = ACTION_NONE;
	dat->actions->param = NULL;
	dat->actions->next = NULL;
	dat->actions->cancel_keep = 0;
  }
  dat->last_action = dat->actions;
  int ret = libsieve_run(priv);
  if (!prdr_has_failed(priv)) {
    action_list_t *a = dat->actions;
    while (a != NULL) {
      switch (a->a) {
      case ACTION_FILEINTO:
      case ACTION_NOTIFY:
      case ACTION_NONE:
      case ACTION_NULL:
	break;
      case ACTION_DISCARD:
	sieve_discard (priv);
	break;
      case ACTION_REJECT:
	sieve_reject (a->u.rej, priv);
	break;
      case ACTION_REDIRECT:
	sieve_redirect (a->u.red, priv);
	break;
      case ACTION_VACATION:
	sieve_vacation (a->u.vac.autoresp, a->u.vac.send, priv);
	break;
      case ACTION_KEEP:
	sieve_keep (priv);
	break;
      }
      a = a->next;
    }
  }
  free_action_list(dat->actions);
  return ret;
}
