/*
 * Copyright (c) 1983 Eric P. Allman
 * Copyright (c) 1988 Regents of the University of California.
 * All rights reserved.
 *
 * %sccs.include.redist.c%
 */

#ifndef lint
#ifdef USERDB
static char sccsid [] = "@(#)udb.c	5.15 (Berkeley) %G% (with USERDB)";
#else
static char sccsid [] = "@(#)udb.c	5.15 (Berkeley) %G% (without USERDB)";
#endif
#endif

#include "sendmail.h"

#ifdef USERDB

#include <sys/file.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <db.h>

/*
**  UDBEXPAND.C -- interface between sendmail and Berkeley User Data Base.
**
**	This depends on the 4.4BSD db package.
*/


struct udbent
{
	char	*udb_spec;		/* string version of spec */
	int	udb_type;		/* type of entry */
	char	*udb_default;		/* default host for outgoing mail */
	union
	{
		/* type UE_REMOTE -- do remote call for lookup */
		struct
		{
			struct sockaddr_in _udb_addr;	/* address */
			int		_udb_timeout;	/* timeout */
		} udb_remote;
#define udb_addr	udb_u.udb_remote._udb_addr
#define udb_timeout	udb_u.udb_remote._udb_timeout

		/* type UE_FORWARD -- forward message to remote */
		struct
		{
			char	*_udb_fwdhost;	/* name of forward host */
		} udb_forward;
#define udb_fwdhost	udb_u.udb_forward._udb_fwdhost

		/* type UE_FETCH -- lookup in local database */
		struct
		{
			char	*_udb_dbname;	/* pathname of database */
			DB	*_udb_dbp;	/* open database ptr */
		} udb_lookup;
#define udb_dbname	udb_u.udb_lookup._udb_dbname
#define udb_dbp		udb_u.udb_lookup._udb_dbp
	} udb_u;
};

#define UDB_EOLIST	0	/* end of list */
#define UDB_SKIP	1	/* skip this entry */
#define UDB_REMOTE	2	/* look up in remote database */
#define UDB_DBFETCH	3	/* look up in local database */
#define UDB_FORWARD	4	/* forward to remote host */

#define MAXUDBENT	10	/* maximum number of UDB entries */


struct option
{
	char	*name;
	char	*val;
};
/*
**  UDBEXPAND -- look up user in database and expand
**
**	Parameters:
**		a -- address to expand.
**		sendq -- pointer to head of sendq to put the expansions in.
**
**	Returns:
**		EX_TEMPFAIL -- if something "odd" happened -- probably due
**			to accessing a file on an NFS server that is down.
**		EX_OK -- otherwise.
**
**	Side Effects:
**		Modifies sendq.
*/

int	UdbPort = 1616;
int	UdbTimeout = 10;

STATIC struct udbent	UdbEnts[MAXUDBENT + 1];
STATIC int		UdbSock = -1;
STATIC bool		UdbInitialized = FALSE;

int
udbexpand(a, sendq)
	register ADDRESS *a;
	ADDRESS **sendq;
{
	int i;
	register char *p;
	DBT key;
	DBT info;
	bool breakout;
	register struct udbent *up;
	int keylen;
	char keybuf[128];
	char buf[8192];

	if (tTd(28, 1))
		printf("expand(%s)\n", a->q_paddr);

	/* make certain we are supposed to send to this address */
	if (bitset(QDONTSEND, a->q_flags))
		return EX_OK;
	CurEnv->e_to = a->q_paddr;

	/* on first call, locate the database */
	if (!UdbInitialized)
	{
		extern int _udbx_init();

		if (_udbx_init() == EX_TEMPFAIL)
			return EX_TEMPFAIL;
	}

	/* short circuit the process if no chance of a match */
	if (UdbSpec == NULL || UdbSpec[0] == '\0')
		return EX_OK;

	/* if name is too long, assume it won't match */
	if (strlen(a->q_user) > sizeof keybuf - 12)
		return EX_OK;

	/* if name begins with a colon, it indicates our metadata */
	if (a->q_user[0] == ':')
		return EX_OK;

	/* build actual database key */
	(void) strcpy(keybuf, a->q_user);
	(void) strcat(keybuf, ":maildrop");
	keylen = strlen(keybuf);

	breakout = FALSE;
	for (up = UdbEnts; !breakout; up++)
	{
		char *user;
		struct timeval timeout;
		fd_set fdset;

		/*
		**  Select action based on entry type.
		**
		**	On dropping out of this switch, "class" should
		**	explain the type of the data, and "user" should
		**	contain the user information.
		*/

		switch (up->udb_type)
		{
		  case UDB_DBFETCH:
			key.data = keybuf;
			key.size = keylen;
			i = (*up->udb_dbp->seq)(up->udb_dbp, &key, &info, R_CURSOR);
			if (i > 0 || info.size <= 0)
			{
				if (tTd(28, 2))
					printf("expand: no match on %s\n", keybuf);
				continue;
			}

			while (i == 0 && key.size == keylen &&
					bcmp(key.data, keybuf, keylen) == 0)
			{
				breakout = TRUE;
				if (info.size < sizeof buf)
					user = buf;
				else
					user = xalloc(info.size + 1);
				bcopy(info.data, user, info.size);
				user[info.size] = '\0';

				message(Arpa_Info, "expanded to %s", user);
				AliasLevel++;
				sendtolist(user, a, sendq);
				AliasLevel--;

				if (user != buf)
					free(user);

				/* get the next record */
				i = (*up->udb_dbp->seq)(up->udb_dbp, &key, &info, R_NEXT);
			}
			if (i < 0)
			{
				syserr("udbexpand: db-get stat %s");
				return EX_TEMPFAIL;
			}
			break;

		  case UDB_REMOTE:
			/* not yet implemented */
			continue;

		  case UDB_FORWARD:
			i = strlen(up->udb_fwdhost) + strlen(a->q_user) + 1;
			if (i < sizeof buf)
				user = buf;
			else
				user = xalloc(i + 1);
			(void) sprintf(user, "%s@%s", a->q_user, up->udb_fwdhost);
			message(Arpa_Info, "expanded to %s", user);
			AliasLevel++;
			sendtolist(user, a, sendq);
			AliasLevel--;
			if (user != buf)
				free(user);
			breakout = TRUE;
			break;

		  case UDB_EOLIST:
			breakout = TRUE;
			continue;

		  default:
			/* unknown entry type */
			continue;
		}
	}
	return EX_OK;
}
/*
**  UDBSENDER -- return canonical external name of sender, given local name
**
**	Parameters:
**		sender -- the name of the sender on the local machine.
**
**	Returns:
**		The external name for this sender, if derivable from the
**			database.
**		NULL -- if nothing is changed from the database.
**
**	Side Effects:
**		none.
*/

char *
udbsender(sender)
	char *sender;
{
	register char *p;
	register struct udbent *up;
	int i;
	int keylen;
	DBT key, info;
	char keybuf[128];

	if (tTd(28, 1))
		printf("udbsender(%s)\n", sender);

	if (!UdbInitialized)
	{
		if (_udbx_init() == EX_TEMPFAIL)
			return NULL;
	}

	/* short circuit if no spec */
	if (UdbSpec == NULL || UdbSpec[0] == '\0')
		return NULL;

	/* long names can never match and are a pain to deal with */
	if (strlen(sender) > sizeof keybuf - 12)
		return NULL;

	/* names beginning with colons indicate metadata */
	if (sender[0] == ':')
		return NULL;

	/* build database key */
	(void) strcpy(keybuf, sender);
	(void) strcat(keybuf, ":mailname");
	keylen = strlen(keybuf);

	for (up = UdbEnts; up->udb_type != UDB_EOLIST; up++)
	{
		/*
		**  Select action based on entry type.
		*/

		switch (up->udb_type)
		{
		  case UDB_DBFETCH:
			key.data = keybuf;
			key.size = keylen;
			i = (*up->udb_dbp->get)(up->udb_dbp, &key, &info, 0);
			if (i != 0 || info.size <= 0)
			{
				if (tTd(28, 2))
					printf("udbsender: no match on %s\n",
							keybuf);
				continue;
			}

			p = xalloc(info.size + 1);
			bcopy(info.data, p, info.size);
			p[info.size] = '\0';
			if (tTd(28, 1))
				printf("udbsender ==> %s\n", p);
			return p;
		}
	}

	/*
	**  Nothing yet.  Search again for a default case.  But only
	**  use it if we also have a forward (:maildrop) pointer already
	**  in the database.
	*/

	/* build database key */
	(void) strcpy(keybuf, sender);
	(void) strcat(keybuf, ":maildrop");
	keylen = strlen(keybuf);

	for (up = UdbEnts; up->udb_type != UDB_EOLIST; up++)
	{
		switch (up->udb_type)
		{
		  case UDB_DBFETCH:
			/* get the default case for this database */
			if (up->udb_default == NULL)
			{
				key.data = ":default:mailname";
				key.size = strlen(key.data);
				i = (*up->udb_dbp->get)(up->udb_dbp, &key, &info, 0);
				if (i != 0 || info.size <= 0)
				{
					/* no default case */
					up->udb_default = "";
					continue;
				}

				/* save the default case */
				up->udb_default = xalloc(info.size + 1);
				bcopy(info.data, up->udb_default, info.size);
				up->udb_default[info.size] = '\0';
			}
			else if (up->udb_default[0] == '\0')
				continue;

			/* we have a default case -- verify user:maildrop */
			key.data = keybuf;
			key.size = keylen;
			i = (*up->udb_dbp->get)(up->udb_dbp, &key, &info, 0);
			if (i != 0 || info.size <= 0)
			{
				/* nope -- no aliasing for this user */
				continue;
			}

			/* they exist -- build the actual address */
			p = xalloc(strlen(sender) + strlen(up->udb_default) + 2);
			(void) strcpy(p, sender);
			(void) strcat(p, "@");
			(void) strcat(p, up->udb_default);
			if (tTd(28, 1))
				printf("udbsender ==> %s\n", p);
			return p;
		}
	}

	/* still nothing....  too bad */
	return NULL;
}
/*
**  _UDBX_INIT -- parse the UDB specification, opening any valid entries.
**
**	Parameters:
**		none.
**
**	Returns:
**		EX_TEMPFAIL -- if it appeared it couldn't get hold of a
**			database due to a host being down or some similar
**			(recoverable) situation.
**		EX_OK -- otherwise.
**
**	Side Effects:
**		Fills in the UdbEnts structure from UdbSpec.
*/

#define MAXUDBOPTS	27

STATIC int
_udbx_init()
{
	register char *p;
	int i;
	register struct udbent *up;
	char buf[8192];

	if (UdbInitialized)
		return EX_OK;

# ifdef UDB_DEFAULT_SPEC
	if (UdbSpec == NULL)
		UdbSpec = UDB_DEFAULT_SPEC;
# endif

	p = UdbSpec;
	up = UdbEnts;
	while (p != NULL)
	{
		char *spec;
		auto int rcode;
		int nopts;
		int nmx;
		register struct hostent *h;
		char *mxhosts[MAXMXHOSTS + 1];
		struct option opts[MAXUDBOPTS + 1];

		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		if (*p == '\0')
			break;
		spec = p;
		p = index(p, ',');
		if (p != NULL)
			*p++ = '\0';

		/* extract options */
		nopts = _udb_parsespec(spec, opts, MAXUDBOPTS);

		/*
		**  Decode database specification.
		**
		**	In the sendmail tradition, the leading character
		**	defines the semantics of the rest of the entry.
		**
		**	+hostname --	send a datagram to the udb server
		**			on host "hostname" asking for the
		**			home mail server for this user.
		**	*hostname --	similar to +hostname, except that the
		**			hostname is searched as an MX record;
		**			resulting hosts are searched as for
		**			+mxhostname.  If no MX host is found,
		**			this is the same as +hostname.
		**	@hostname --	forward email to the indicated host.
		**			This should be the last in the list,
		**			since it always matches the input.
		**	/dbname	 --	search the named database on the local
		**			host using the Berkeley db package.
		*/

		switch (*spec)
		{
		  case '+':	/* search remote database */
		  case '*':	/* search remote database (expand MX) */
			if (*spec == '*')
			{
				nmx = getmxrr(spec + 1, mxhosts, "", &rcode);
				if (tTd(28, 16))
				{
					int i;

					printf("getmxrr(%s): %d", spec + 1, nmx);
					for (i = 0; i <= nmx; i++)
						printf(" %s", mxhosts[i]);
					printf("\n");
				}
			}
			else
			{
				nmx = 1;
				mxhosts[0] = spec + 1;
			}

			for (i = 0; i < nmx; i++)
			{
				h = gethostbyname(mxhosts[i]);
				if (h == NULL)
					continue;
				up->udb_type = UDB_REMOTE;
				up->udb_addr.sin_family = h->h_addrtype;
				bcopy(h->h_addr_list[0],
				      (char *) &up->udb_addr.sin_addr,
				      h->h_length);
				up->udb_addr.sin_port = UdbPort;
				up->udb_timeout = UdbTimeout;
				up++;
			}

			/* set up a datagram socket */
			if (UdbSock < 0)
			{
				UdbSock = socket(AF_INET, SOCK_DGRAM, 0);
				(void) fcntl(UdbSock, F_SETFD, 1);
			}
			break;

		  case '@':	/* forward to remote host */
			up->udb_type = UDB_FORWARD;
			up->udb_fwdhost = spec + 1;
			up++;
			break;

		  case '/':	/* look up remote name */
			up->udb_dbname = spec;
			errno = 0;
			up->udb_dbp = dbopen(spec, O_RDONLY, 0644, DB_BTREE, NULL);
			if (up->udb_dbp == NULL)
			{
				if (errno != ENOENT && errno != EACCES)
				{
					up->udb_type = UDB_EOLIST;
					goto tempfail;
				}
				break;
			}
			up->udb_type = UDB_DBFETCH;
			up++;
			break;
		}
	}
	up->udb_type = UDB_EOLIST;

	if (tTd(28, 4))
	{
		for (up = UdbEnts; up->udb_type != UDB_EOLIST; up++)
		{
			switch (up->udb_type)
			{
			  case UDB_REMOTE:
				printf("REMOTE: addr %s, timeo %d\n",
					inet_ntoa(up->udb_addr.sin_addr),
					up->udb_timeout);
				break;

			  case UDB_DBFETCH:
				printf("FETCH: file %s\n",
					up->udb_dbname);
				break;

			  case UDB_FORWARD:
				printf("FORWARD: host %s\n",
					up->udb_fwdhost);
				break;

			  default:
				printf("UNKNOWN\n");
				break;
			}
		}
	}

	UdbInitialized = TRUE;
	return EX_OK;

	/*
	**  On temporary failure, back out anything we've already done
	*/

  tempfail:
	for (up = UdbEnts; up->udb_type != UDB_EOLIST; up++)
	{
		if (up->udb_type == UDB_DBFETCH)
		{
			(*up->udb_dbp->close)(up->udb_dbp);
		}
	}
	return EX_TEMPFAIL;
}

int
_udb_parsespec(udbspec, opt, maxopts)
	char *udbspec;
	struct option opt[];
	int maxopts;
{
	register char *spec;
	register char *spec_end;
	register int optnum;

	spec_end = index(udbspec, ':');
	for (optnum = 0; optnum < maxopts && (spec = spec_end) != NULL; optnum++)
	{
		register char *p;

		while (isspace(*spec))
			spec++;
		spec_end = index(spec, ':');
		if (spec_end != NULL)
			*spec_end++ = '\0';

		opt[optnum].name = spec;
		opt[optnum].val = NULL;
		p = index(spec, '=');
		if (p != NULL)
			opt[optnum].val = ++p;
	}
	return optnum;
}

#else /* not USERDB */

int
udbexpand(a, sendq)
	ADDRESS *a;
	ADDRESS **sendq;
{
	return EX_OK;
}

#endif /* USERDB */
