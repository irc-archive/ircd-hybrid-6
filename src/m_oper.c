/************************************************************************
 *   IRC - Internet Relay Chat, src/m_oper.c
 *   Copyright (C) 1990 Jarkko Oikarinen and
 *                      University of Oulu, Computing Center
 *
 *   See file AUTHORS in IRC package for additional names of
 *   the programmers. 
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *   $Id$
 */

#include "m_commands.h"
#include "ircd.h"
#include "numeric.h"
#include "s_serv.h"
#include "send.h"
#include "fdlist.h"
#include "s_conf.h"
#include "struct.h"
#include "client.h"
#include "irc_string.h"

#include <fcntl.h>

static char buf[BUFSIZE];

/*
 * m_functions execute protocol messages on this server:
 *
 *      cptr    is always NON-NULL, pointing to a *LOCAL* client
 *              structure (with an open socket connected!). This
 *              identifies the physical socket where the message
 *              originated (or which caused the m_function to be
 *              executed--some m_functions may call others...).
 *
 *      sptr    is the source of the message, defined by the
 *              prefix part of the message if present. If not
 *              or prefix not found, then sptr==cptr.
 *
 *              (!IsServer(cptr)) => (cptr == sptr), because
 *              prefixes are taken *only* from servers...
 *
 *              (IsServer(cptr))
 *                      (sptr == cptr) => the message didn't
 *                      have the prefix.
 *
 *                      (sptr != cptr && IsServer(sptr) means
 *                      the prefix specified servername. (?)
 *
 *                      (sptr != cptr && !IsServer(sptr) means
 *                      that message originated from a remote
 *                      user (not local).
 *
 *              combining
 *
 *              (!IsServer(sptr)) means that, sptr can safely
 *              taken as defining the target structure of the
 *              message in this server.
 *
 *      *Always* true (if 'parse' and others are working correct):
 *
 *      1)      sptr->from == cptr  (note: cptr->from == cptr)
 *
 *      2)      MyConnect(sptr) <=> sptr == cptr (e.g. sptr
 *              *cannot* be a local connection, unless it's
 *              actually cptr!). [MyConnect(x) should probably
 *              be defined as (x == x->from) --msa ]
 *
 *      parc    number of variable parameter strings (if zero,
 *              parv is allowed to be NULL)
 *
 *      parv    a NULL terminated list of parameter pointers,
 *
 *                      parv[0], sender (prefix string), if not present
 *                              this points to an empty string.
 *                      parv[1]...parv[parc-1]
 *                              pointers to additional parameters
 *                      parv[parc] == NULL, *always*
 *
 *              note:   it is guaranteed that parv[0]..parv[parc-1] are all
 *                      non-NULL pointers.
 */

/*
** m_oper
**      parv[0] = sender prefix
**      parv[1] = oper name
**      parv[2] = oper password
*/
int     m_oper(struct Client *cptr,
               struct Client *sptr,
               int parc,
               char *parv[])
{
  struct ConfItem *aconf;
  char  *name, *password, *encr;
#ifdef CRYPT_OPER_PASSWORD
  extern        char *crypt();
#endif /* CRYPT_OPER_PASSWORD */
  char *operprivs;

  name = parc > 1 ? parv[1] : (char *)NULL;
  password = parc > 2 ? parv[2] : (char *)NULL;

  if (!IsServer(cptr) && (EmptyString(name) || EmptyString(password)))
    {
      sendto_one(sptr, form_str(ERR_NEEDMOREPARAMS),
                 me.name, parv[0], "OPER");
      return 0;
    }
        
  /* if message arrived from server, trust it, and set to oper */
  
  if ((IsServer(cptr) || IsMe(cptr)) && !IsOper(sptr))
    {
      sptr->flags |= FLAGS_OPER;
      Count.oper++;
      sendto_serv_butone(cptr, ":%s MODE %s :+o", parv[0], parv[0]);
      if (IsMe(cptr))
        sendto_one(sptr, form_str(RPL_YOUREOPER),
                   me.name, parv[0]);
      return 0;
    }
  else if (IsAnOper(sptr))
    {
      if (MyConnect(sptr))
        {
          sendto_one(sptr, form_str(RPL_YOUREOPER),
                     me.name, parv[0]);
          SendMessageFile(sptr, &ConfigFileEntry.opermotd);
        }
      return 0;
    }
  if (!(aconf = find_conf_exact(name, sptr->username, sptr->host,
                                CONF_OPS)) &&
      !(aconf = find_conf_exact(name, sptr->username,
                                inetntoa((char *)&cptr->ip), CONF_OPS)))
    {
      sendto_one(sptr, form_str(ERR_NOOPERHOST), me.name, parv[0]);
#if defined(FAILED_OPER_NOTICE) && defined(SHOW_FAILED_OPER_ID)
#ifdef SHOW_FAILED_OPER_PASSWD
      sendto_realops("Failed OPER attempt [%s(%s)] - identity mismatch: %s [%s@%s]",
        name, password, sptr->name, sptr->username, sptr->host);
#else
      sendto_realops("Failed OPER attempt - host mismatch by %s (%s@%s)",
                     parv[0], sptr->username, sptr->host);
#endif /* SHOW_FAILED_OPER_PASSWD */
#endif /* FAILED_OPER_NOTICE && SHOW_FAILED_OPER_ID */
      return 0;
    }
#ifdef CRYPT_OPER_PASSWORD
  /* use first two chars of the password they send in as salt */

  /* passwd may be NULL pointer. Head it off at the pass... */
  if (password && *aconf->passwd)
    encr = crypt(password, aconf->passwd);
  else
    encr = "";
#else
  encr = password;
#endif  /* CRYPT_OPER_PASSWORD */

  if ((aconf->status & CONF_OPS) &&
      0 == strcmp(encr, aconf->passwd) && !attach_conf(sptr, aconf))
    {
      int old = (sptr->umodes & ALL_UMODES);
      
      if (aconf->status == CONF_LOCOP)
        {
          SetLocOp(sptr);
          if((int)aconf->hold)
            {
              sptr->umodes |= ((int)aconf->hold & ALL_UMODES); 
              sendto_one(sptr, ":%s NOTICE %s:*** Oper flags set from conf",
                         me.name,parv[0]);
            }
          else
            {
              sptr->umodes |= (LOCOP_UMODES);
            }

          /* A local oper can't global kill ever, or do remote re-routes
           * or glines. Make sure thats enforced here.
           */

          sptr->port &= 
            ~(CONF_OPER_GLOBAL_KILL|CONF_OPER_REMOTE|CONF_OPER_GLINE);
        }
      else
        {
          SetOper(sptr);
          if((int)aconf->hold)
            {
              sptr->umodes |= ((int)aconf->hold & ALL_UMODES); 
              if( !IsSetOperN(sptr) )
                sptr->umodes &= ~FLAGS_NCHANGE;
              
              sendto_one(sptr, ":%s NOTICE %s:*** Oper flags set from conf",
                         me.name,parv[0]);
            }
          else
            {
              sptr->umodes |= (OPER_UMODES);
            }
        }
      SetIPHidden(sptr);
      Count.oper++;

      SetElined(cptr);
      
      /* LINKLIST */  
      /* add to oper link list -Dianora */
      cptr->next_oper_client = oper_cptr_list;
      oper_cptr_list = cptr;

      if(cptr->confs)
        {
          struct ConfItem *aconf;
          aconf = cptr->confs->value.aconf;
          operprivs = oper_privs_as_string(cptr,aconf->port);
        }
      else
        operprivs = "";

      fdlist_add(sptr->fd, FDL_OPER | FDL_BUSY);
#ifdef CUSTOM_ERR
      sendto_ops("%s (%s@%s) has just acquired the personality of a petty megalomaniacal tyrant [IRC(%c)p]", parv[0],
#else
      sendto_ops("%s (%s@%s) is now operator (%c)", parv[0],
#endif /* CUSTOM_ERR */
                 sptr->username, sptr->host,
                 IsOper(sptr) ? 'O' : 'o');
      send_umode_out(cptr, sptr, old);
      sendto_one(sptr, form_str(RPL_YOUREOPER), me.name, parv[0]);
      sendto_one(sptr, ":%s NOTICE %s:*** Oper privs are %s",me.name,parv[0],
                 operprivs);

      SendMessageFile(sptr, &ConfigFileEntry.opermotd);

#if !defined(CRYPT_OPER_PASSWORD) && (defined(FNAME_OPERLOG) ||\
    (defined(USE_SYSLOG) && defined(SYSLOG_OPER)))
        encr = "";
#endif
#if defined(USE_SYSLOG) && defined(SYSLOG_OPER)
        syslog(LOG_INFO, "OPER (%s) (%s) by (%s!%s@%s)",
               name, encr,
               parv[0], sptr->username, sptr->host);
#endif
#ifdef FNAME_OPERLOG
        {
          int     logfile;

          /*
           * This conditional makes the logfile active only after
           * it's been created - thus logging can be turned off by
           * removing the file.
           *
           */

          if (IsPerson(sptr) &&
              (logfile = open(FNAME_OPERLOG, O_WRONLY|O_APPEND)) != -1)
            {
              /* (void)alarm(0); */
              ircsprintf(buf, "%s OPER (%s) (%s) by (%s!%s@%s)\n",
                               myctime(CurrentTime), name, encr,
                               parv[0], sptr->username,
                               sptr->host);
              (void)write(logfile, buf, strlen(buf));
              (void)close(logfile);
            }
        }
#endif
    }
  else
    {
      (void)detach_conf(sptr, aconf);
      sendto_one(sptr,form_str(ERR_PASSWDMISMATCH),me.name, parv[0]);
#ifdef FAILED_OPER_NOTICE
#ifdef SHOW_FAILED_OPER_PASSWD
      sendto_realops("Failed OPER attempt [%s(%s)] - passwd mismatch: %s [%s@%s]",
        name, password, sptr->name, sptr->username, sptr->host);
#else
      sendto_realops("Failed OPER attempt by %s (%s@%s)",
                     parv[0], sptr->username, sptr->host);
#endif /* SHOW_FAILED_OPER_PASSWD */
#endif
    }
  return 0;
}
