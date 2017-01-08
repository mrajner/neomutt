/*
 * Copyright (C) 1996-2000,2006-2007,2010 Michael R. Elkins <me@mutt.org>, and others
 * 
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */ 

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "mapping.h"
#include "keymap.h"
#include "mailbox.h"
#include "copy.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>

#include "mutt_crypt.h"
#include "mutt_curses.h"
#include "group.h"
#include "mutt_menu.h"

#ifdef USE_IMAP
#include "mx.h"
#include "imap/imap.h"
#endif

#ifdef USE_NOTMUCH
#include "mutt_notmuch.h"
#endif

static int eat_regexp (pattern_t *pat, BUFFER *, BUFFER *);
static int eat_date (pattern_t *pat, BUFFER *, BUFFER *);
static int eat_range (pattern_t *pat, BUFFER *, BUFFER *);
static int patmatch (const pattern_t *pat, const char *buf);

static const struct pattern_flags
{
  int tag;	/* character used to represent this op */
  int op;	/* operation to perform */
  int class;
  int (*eat_arg) (pattern_t *, BUFFER *, BUFFER *);
}
Flags[] =
{
  { 'A', MUTT_ALL,			0,		NULL },
  { 'b', MUTT_BODY,		MUTT_FULL_MSG,	eat_regexp },
  { 'B', MUTT_WHOLE_MSG,		MUTT_FULL_MSG,	eat_regexp },
  { 'c', MUTT_CC,			0,		eat_regexp },
  { 'C', MUTT_RECIPIENT,		0,		eat_regexp },
  { 'd', MUTT_DATE,		0,		eat_date },
  { 'D', MUTT_DELETED,		0,		NULL },
  { 'e', MUTT_SENDER,		0,		eat_regexp },
  { 'E', MUTT_EXPIRED,		0,		NULL },
  { 'f', MUTT_FROM,		0,		eat_regexp },
  { 'F', MUTT_FLAG,		0,		NULL },
  { 'g', MUTT_CRYPT_SIGN,		0,		NULL },
  { 'G', MUTT_CRYPT_ENCRYPT,	0,		NULL },
  { 'h', MUTT_HEADER,		MUTT_FULL_MSG,	eat_regexp },
  { 'H', MUTT_HORMEL,		0,		eat_regexp },
  { 'i', MUTT_ID,			0,		eat_regexp },
  { 'k', MUTT_PGP_KEY,		0,		NULL },
  { 'l', MUTT_LIST,		0,		NULL },
  { 'L', MUTT_ADDRESS,		0,		eat_regexp },
  { 'm', MUTT_MESSAGE,		0,		eat_range },
  { 'n', MUTT_SCORE,		0,		eat_range },
  { 'N', MUTT_NEW,			0,		NULL },
  { 'O', MUTT_OLD,			0,		NULL },
  { 'p', MUTT_PERSONAL_RECIP,	0,		NULL },
  { 'P', MUTT_PERSONAL_FROM,	0,		NULL },
  { 'Q', MUTT_REPLIED,		0,		NULL },
  { 'r', MUTT_DATE_RECEIVED,	0,		eat_date },
  { 'R', MUTT_READ,		0,		NULL },
  { 's', MUTT_SUBJECT,		0,		eat_regexp },
  { 'S', MUTT_SUPERSEDED,		0,		NULL },
  { 't', MUTT_TO,			0,		eat_regexp },
  { 'T', MUTT_TAG,			0,		NULL },
  { 'u', MUTT_SUBSCRIBED_LIST,	0,		NULL },
  { 'U', MUTT_UNREAD,		0,		NULL },
  { 'v', MUTT_COLLAPSED,		0,		NULL },
  { 'V', MUTT_CRYPT_VERIFIED,	0,		NULL },
#ifdef USE_NNTP
  { 'w', MUTT_NEWSGROUPS,		0,		eat_regexp },
#endif
  { 'x', MUTT_REFERENCE,		0,		eat_regexp },
  { 'X', MUTT_MIMEATTACH,		0,		eat_range },
  { 'y', MUTT_XLABEL,		0,		eat_regexp },
#ifdef USE_NOTMUCH
  { 'Y', MUTT_NOTMUCH_LABEL,	0,		eat_regexp },
#endif
  { 'z', MUTT_SIZE,		0,		eat_range },
  { '=', MUTT_DUPLICATED,		0,		NULL },
  { '$', MUTT_UNREFERENCED,	0,		NULL },
  { 0,   0,			0,		NULL }
};

static pattern_t *SearchPattern = NULL; /* current search pattern */
static char LastSearch[STRING] = { 0 };	/* last pattern searched for */
static char LastSearchExpn[LONG_STRING] = { 0 }; /* expanded version of
						    LastSearch */

#define MUTT_MAXRANGE -1

/* constants for parse_date_range() */
#define MUTT_PDR_NONE	0x0000
#define MUTT_PDR_MINUS	0x0001
#define MUTT_PDR_PLUS	0x0002
#define MUTT_PDR_WINDOW	0x0004
#define MUTT_PDR_ABSOLUTE	0x0008
#define MUTT_PDR_DONE	0x0010
#define MUTT_PDR_ERROR	0x0100
#define MUTT_PDR_ERRORDONE	(MUTT_PDR_ERROR | MUTT_PDR_DONE)


/* if no uppercase letters are given, do a case-insensitive search */
int mutt_which_case (const char *s)
{
  wchar_t w;
  mbstate_t mb;
  size_t l;
  
  memset (&mb, 0, sizeof (mb));
  
  for (; (l = mbrtowc (&w, s, MB_CUR_MAX, &mb)) != 0; s += l)
  {
    if (l == (size_t) -2)
      continue; /* shift sequences */
    if (l == (size_t) -1)
      return 0; /* error; assume case-sensitive */
    if (iswalpha ((wint_t) w) && iswupper ((wint_t) w))
      return 0; /* case-sensitive */
  }

  return REG_ICASE; /* case-insensitive */
}

static int
msg_search (CONTEXT *ctx, pattern_t* pat, int msgno)
{
  MESSAGE *msg = NULL;
  STATE s;
  FILE *fp = NULL;
  long lng = 0;
  int match = 0;
  HEADER *h = ctx->hdrs[msgno];
  char *buf;
  size_t blen;
#ifdef USE_FMEMOPEN
  char *temp;
  size_t tempsize;
#else
  char tempfile[_POSIX_PATH_MAX];
  struct stat st;
#endif

  if ((msg = mx_open_message (ctx, msgno)) != NULL)
  {
    if (option (OPTTHOROUGHSRC))
    {
      /* decode the header / body */
      memset (&s, 0, sizeof (s));
      s.fpin = msg->fp;
      s.flags = MUTT_CHARCONV;
#ifdef USE_FMEMOPEN
      s.fpout = open_memstream (&temp, &tempsize);
      if (!s.fpout) {
	mutt_perror ("Error opening memstream");
	return 0;
      }
#else
      mutt_mktemp (tempfile, sizeof (tempfile));
      if ((s.fpout = safe_fopen (tempfile, "w+")) == NULL)
      {
	mutt_perror (tempfile);
	return (0);
      }
#endif

      if (pat->op != MUTT_BODY)
	mutt_copy_header (msg->fp, h, s.fpout, CH_FROM | CH_DECODE, NULL);

      if (pat->op != MUTT_HEADER)
      {
	mutt_parse_mime_message (ctx, h);

	if (WithCrypto && (h->security & ENCRYPT)
            && !crypt_valid_passphrase(h->security))
	{
	  mx_close_message (ctx, &msg);
	  if (s.fpout)
	  {
	    safe_fclose (&s.fpout);
#ifdef USE_FMEMOPEN
            FREE(&temp);
#else
	    unlink (tempfile);
#endif
	  }
	  return (0);
	}

	fseeko (msg->fp, h->offset, 0);
	mutt_body_handler (h->content, &s);
      }

#ifdef USE_FMEMOPEN
      fclose (s.fpout);
      lng = tempsize;

      if (tempsize) {
        fp = fmemopen (temp, tempsize, "r");
        if (!fp) {
          mutt_perror ("Error re-opening memstream");
          return 0;
        }
      } else { /* fmemopen cannot handle empty buffers */
        fp = safe_fopen ("/dev/null", "r");
        if (!fp) {
          mutt_perror ("Error opening /dev/null");
          return 0;
        }
      }
#else
      fp = s.fpout;
      fflush (fp);
      fseek (fp, 0, 0);
      fstat (fileno (fp), &st);
      lng = (long) st.st_size;
#endif
    }
    else
    {
      /* raw header / body */
      fp = msg->fp;
      if (pat->op != MUTT_BODY)
      {
	fseeko (fp, h->offset, 0);
	lng = h->content->offset - h->offset;
      }
      if (pat->op != MUTT_HEADER)
      {
	if (pat->op == MUTT_BODY)
	  fseeko (fp, h->content->offset, 0);
	lng += h->content->length;
      }
    }

    blen = STRING;
    buf = safe_malloc (blen);

    /* search the file "fp" */
    while (lng > 0)
    {
      if (pat->op == MUTT_HEADER)
      {
	if (*(buf = mutt_read_rfc822_line (fp, buf, &blen)) == '\0')
	  break;
      }
      else if (fgets (buf, blen - 1, fp) == NULL)
	break; /* don't loop forever */
      if (patmatch (pat, buf) == 0)
      {
	match = 1;
	break;
      }
      lng -= mutt_strlen (buf);
    }

    FREE (&buf);
    
    mx_close_message (ctx, &msg);

    if (option (OPTTHOROUGHSRC))
    {
      safe_fclose (&fp);
#ifdef USE_FMEMOPEN
      if (tempsize)
        FREE(&temp);
#else
      unlink (tempfile);
#endif
    }
  }

  return match;
}

static int eat_regexp (pattern_t *pat, BUFFER *s, BUFFER *err)
{
  BUFFER buf;
  char errmsg[STRING];
  int r;
  char *pexpr;

  mutt_buffer_init (&buf);
  pexpr = s->dptr;
  if (mutt_extract_token (&buf, s, MUTT_TOKEN_PATTERN | MUTT_TOKEN_COMMENT) != 0 ||
      !buf.data)
  {
    snprintf (err->data, err->dsize, _("Error in expression: %s"), pexpr);
    return (-1);
  }
  if (!*buf.data)
  {
    snprintf (err->data, err->dsize, _("Empty expression"));
    return (-1);
  }

#if 0
  /* If there are no RE metacharacters, use simple search anyway */
  if (!pat->stringmatch && !strpbrk (buf.data, "|[{.*+?^$"))
    pat->stringmatch = 1;
#endif

  if (pat->stringmatch)
  {
    pat->p.str = safe_strdup (buf.data);
    pat->ign_case = mutt_which_case (buf.data) == REG_ICASE;
    FREE (&buf.data);
  }
  else if (pat->groupmatch)
  {
    pat->p.g = mutt_pattern_group (buf.data);
    FREE (&buf.data);
  }
  else
  {
    pat->p.rx = safe_malloc (sizeof (regex_t));
    r = REGCOMP (pat->p.rx, buf.data, REG_NEWLINE | REG_NOSUB | mutt_which_case (buf.data));
    if (r)
    {
      regerror (r, pat->p.rx, errmsg, sizeof (errmsg));
      mutt_buffer_printf (err, "'%s': %s", buf.data, errmsg);
      FREE (&buf.data);
      FREE (&pat->p.rx);
      return (-1);
    }
    FREE (&buf.data);
  }

  return 0;
}

#define KILO 1024
#define MEGA 1048576
#define CTX_HUMAN_MSGNO(c) (((c)->hdrs[(c)->v2r[(c)->menu->current]]->msgno)+1)

enum
{
  RANGE_K_REL,
  RANGE_K_ABS,
  /* add new ones HERE */
  RANGE_K_INVALID
};

static int
scan_range_num (BUFFER *s, regmatch_t pmatch[], int group, int kind)
{
  int num;
  unsigned char c;

  /* this cast looks dangerous, but is already all over this code
   * (explicit or not) */
  num = (int)strtol(&s->dptr[pmatch[group].rm_so], NULL, 0);
  c = (unsigned char)(s->dptr[pmatch[group].rm_eo - 1]);
  if (toupper(c) == 'K')
    num *= KILO;
  else if (toupper(c) == 'M')
    num *= MEGA;
  if (kind == RANGE_K_REL)
    num += CTX_HUMAN_MSGNO(Context);
  return num;
}

#define RANGE_DOT '.'
#define RANGE_CIRCUM '^'
#define RANGE_DOLLAR '$'

/* range sides: left or right */
enum
{
  RANGE_S_LEFT,
  RANGE_S_RIGHT
};

static int
scan_range_slot (BUFFER *s, regmatch_t pmatch[], int group,
                 int side, int kind)
{
  unsigned char c;
  static const int empty_val[] =
    {
      [RANGE_S_LEFT] = 1,
      [RANGE_S_RIGHT] = MUTT_MAXRANGE
    };

  /* This means the left or right subpattern was empty, e.g. ",." */
  if (pmatch[group].rm_so == -1)
    return empty_val[side];
  else
  {
    /* We have something, so determine what */
    c = (unsigned char)(s->dptr[pmatch[group].rm_so]);
    switch (c)
    {
    case RANGE_CIRCUM:
      return 1;
    case RANGE_DOLLAR:
      return MUTT_MAXRANGE;
    case RANGE_DOT:
      return CTX_HUMAN_MSGNO(Context);
    default:
      /* Only other possibility: a number */
      return scan_range_num(s, pmatch, group, kind);
    }
  }
}

static void
order_range (pattern_t *pat)
{
  int num;

  if ((pat->min != MUTT_MAXRANGE) && (pat->min <= pat->max))
    return;
  else if ((pat->max == MUTT_MAXRANGE) || (pat->min <= pat->max))
    return;
  num = pat->min;
  pat->min = pat->max;
  pat->max = num;
}

/* Error codes for eat_range_by_regexp */
enum
{
  RANGE_E_OK,
  RANGE_E_SYNTAX,
  RANGE_E_CTX,
};

static int
report_regerror(int regerr, regex_t *preg, BUFFER *err)
{
  size_t ds = err->dsize;

  if (regerror(regerr, preg, err->data, ds) > ds)
    dprint(2, (debugfile, "warning: buffer too small for regerror\n"));
  /* The return value is fixed, exists only to shorten code at callsite */
  return RANGE_E_SYNTAX;
}

static int
is_context_available(BUFFER *s, regmatch_t pmatch[], int kind, BUFFER *err)
{
  char *context_loc;
  const char *context_req_chars[] =
  {
    [RANGE_K_REL] = ".0123456789",
    [RANGE_K_ABS] = "."
  };

  /* First decide if we're going to need the context at all.
   * Relative patterns need it iff they contain a dot or a number.
   * Absolute patterns only need it if they contain a dot. */
  context_loc = strpbrk(s->dptr+pmatch[0].rm_so, context_req_chars[kind]);
  if ((context_loc == NULL) || (context_loc >= &s->dptr[pmatch[0].rm_eo]))
    return 1;

  /* We need a current message.  Do we actually have one? */
  if (Context && Context->menu)
    return 1;

  /* Nope. */
  strfcpy(err->data, _("No current message"), err->dsize);
  return 0;
}

#define RANGE_REL_SLOT_RX \
    "[[:blank:]]*([.^$]|-?([[:digit:]]+|0x[[:xdigit:]]+)[MmKk]?)?[[:blank:]]*"

#define RANGE_REL_RX ("^" RANGE_REL_SLOT_RX "," RANGE_REL_SLOT_RX)

/* Almost the same, but no negative numbers allowed */

#define RANGE_ABS_SLOT_RX \
    "[[:blank:]]*([.^$]|([[:digit:]]+|0x[[:xdigit:]]+)[MmKk]?)?[[:blank:]]*"

#define RANGE_ABS_RX ("^" RANGE_ABS_SLOT_RX "-" RANGE_ABS_SLOT_RX)

#define RANGE_RX_GROUPS 5

struct range_regexp
{
  const char* raw;              /* regexp as string */
  int lgrp;                     /* paren group matching the left side */
  int rgrp;                     /* paren group matching the right side */
  int ready;                    /* compiled yet? */
  regex_t cooked;               /* compiled form */
};

static struct range_regexp range_regexps[] =
{
  [RANGE_K_REL] = {.raw = RANGE_REL_RX, .lgrp = 1, .rgrp = 3, .ready = 0},
  [RANGE_K_ABS] = {.raw = RANGE_ABS_RX, .lgrp = 1, .rgrp = 3, .ready = 0},
};

static int
eat_range_by_regexp (pattern_t *pat, BUFFER *s, int kind, BUFFER *err)
{
  int regerr;
  regmatch_t pmatch[RANGE_RX_GROUPS];
  struct range_regexp *pspec = &range_regexps[kind];

  /* First time through, compile the big regexp */
  if (!pspec->ready)
  {
    regerr = regcomp(&pspec->cooked, pspec->raw, REG_EXTENDED);
    if (regerr)
      return report_regerror(regerr, &pspec->cooked, err);
    pspec->ready = 1;
  }

  /* Match the pattern buffer against the compiled regexp.
   * No match means syntax error. */
  regerr = regexec(&pspec->cooked, s->dptr, RANGE_RX_GROUPS, pmatch, 0);
  if (regerr)
    return report_regerror(regerr, &pspec->cooked, err);

  if (!is_context_available(s, pmatch, kind, err))
    return RANGE_E_CTX;

  /* Snarf the contents of the two sides of the range. */
  pat->min = scan_range_slot(s, pmatch, pspec->lgrp, RANGE_S_LEFT, kind);
  pat->max = scan_range_slot(s, pmatch, pspec->rgrp, RANGE_S_RIGHT, kind);
  dprint(1, (debugfile, "pat->min=%d pat->max=%d\n", pat->min, pat->max));

  /* Since we don't enforce order, we must swap bounds if they're backward */
  order_range(pat);

  /* Slide pointer past the entire match. */
  s->dptr += pmatch[0].rm_eo;
  return RANGE_E_OK;
}

int eat_range (pattern_t *pat, BUFFER *s, BUFFER *err)
{
  int skip_quote = 0;
  int i_kind;

  /*
   * If simple_search is set to "~m %s", the range will have double quotes
   * around it...
   */
  if (*s->dptr == '"')
  {
    s->dptr++;
    skip_quote = 1;
  }

  /* There are just 2 for now, but there'll be more, hence the loop */
  for (i_kind = 0; i_kind != RANGE_K_INVALID; ++i_kind)
  {
    switch (eat_range_by_regexp(pat, s, i_kind, err))
    {
    case RANGE_E_CTX:
      /* This means it matched syntactically but lacked context.
       * No point in continuing. */
      break;
    case RANGE_E_SYNTAX:
      /* Try another syntax, then */
      continue;
    case RANGE_E_OK:
      if (skip_quote && (*s->dptr == '"'))
        s->dptr++;
      SKIPWS (s->dptr);
      return 0;
    }
  }
  return -1;
}

static const char *getDate (const char *s, struct tm *t, BUFFER *err)
{
  char *p;
  time_t now = time (NULL);
  struct tm *tm = localtime (&now);

  t->tm_mday = strtol (s, &p, 10);
  if (t->tm_mday < 1 || t->tm_mday > 31)
  {
    snprintf (err->data, err->dsize, _("Invalid day of month: %s"), s);
    return NULL;
  }
  if (*p != '/')
  {
    /* fill in today's month and year */
    t->tm_mon = tm->tm_mon;
    t->tm_year = tm->tm_year;
    return p;
  }
  p++;
  t->tm_mon = strtol (p, &p, 10) - 1;
  if (t->tm_mon < 0 || t->tm_mon > 11)
  {
    snprintf (err->data, err->dsize, _("Invalid month: %s"), p);
    return NULL;
  }
  if (*p != '/')
  {
    t->tm_year = tm->tm_year;
    return p;
  }
  p++;
  t->tm_year = strtol (p, &p, 10);
  if (t->tm_year < 70) /* year 2000+ */
    t->tm_year += 100;
  else if (t->tm_year > 1900)
    t->tm_year -= 1900;
  return p;
}

/* Ny	years
   Nm	months
   Nw	weeks
   Nd	days */
static const char *get_offset (struct tm *tm, const char *s, int sign)
{
  char *ps;
  int offset = strtol (s, &ps, 0);
  if ((sign < 0 && offset > 0) || (sign > 0 && offset < 0))
    offset = -offset;

  switch (*ps)
  {
    case 'y':
      tm->tm_year += offset;
      break;
    case 'm':
      tm->tm_mon += offset;
      break;
    case 'w':
      tm->tm_mday += 7 * offset;
      break;
    case 'd':
      tm->tm_mday += offset;
      break;
    default:
      return s;
  }
  mutt_normalize_time (tm);
  return (ps + 1);
}

static void adjust_date_range (struct tm *min, struct tm *max)
{
  if (min->tm_year > max->tm_year
      || (min->tm_year == max->tm_year && min->tm_mon > max->tm_mon)
      || (min->tm_year == max->tm_year && min->tm_mon == max->tm_mon
	&& min->tm_mday > max->tm_mday))
  {
    int tmp;
    
    tmp = min->tm_year;
    min->tm_year = max->tm_year;
    max->tm_year = tmp;
      
    tmp = min->tm_mon;
    min->tm_mon = max->tm_mon;
    max->tm_mon = tmp;
      
    tmp = min->tm_mday;
    min->tm_mday = max->tm_mday;
    max->tm_mday = tmp;
    
    min->tm_hour = min->tm_min = min->tm_sec = 0;
    max->tm_hour = 23;
    max->tm_min = max->tm_sec = 59;
  }
}

static const char * parse_date_range (const char* pc, struct tm *min,
    struct tm *max, int haveMin, struct tm *baseMin, BUFFER *err)
{
  int flag = MUTT_PDR_NONE;	
  while (*pc && ((flag & MUTT_PDR_DONE) == 0))
  {
    const char *pt;
    char ch = *pc++;
    SKIPWS (pc);
    switch (ch)
    {
      case '-':
      {
	/* try a range of absolute date minus offset of Ndwmy */
	pt = get_offset (min, pc, -1);
	if (pc == pt)
	{
	  if (flag == MUTT_PDR_NONE)
	  { /* nothing yet and no offset parsed => absolute date? */
	    if (!getDate (pc, max, err))
	      flag |= (MUTT_PDR_ABSOLUTE | MUTT_PDR_ERRORDONE);  /* done bad */
	    else
	    {
	      /* reestablish initial base minimum if not specified */
	      if (!haveMin)
		memcpy (min, baseMin, sizeof(struct tm));
	      flag |= (MUTT_PDR_ABSOLUTE | MUTT_PDR_DONE);  /* done good */
	    }
	  }
	  else
	    flag |= MUTT_PDR_ERRORDONE;
	}
	else
	{
	  pc = pt;
	  if (flag == MUTT_PDR_NONE && !haveMin)
	  { /* the very first "-3d" without a previous absolute date */
	    max->tm_year = min->tm_year;
	    max->tm_mon = min->tm_mon;
	    max->tm_mday = min->tm_mday;
	  }
	  flag |= MUTT_PDR_MINUS;
	}
      }
      break;
      case '+':
      { /* enlarge plusRange */
	pt = get_offset (max, pc, 1);
	if (pc == pt)
	  flag |= MUTT_PDR_ERRORDONE;
	else
	{
	  pc = pt;
	  flag |= MUTT_PDR_PLUS;
	}
      }
      break;
      case '*':
      { /* enlarge window in both directions */
	pt = get_offset (min, pc, -1);
	if (pc == pt)
	  flag |= MUTT_PDR_ERRORDONE;
	else
	{
	  pc = get_offset (max, pc, 1);
	  flag |= MUTT_PDR_WINDOW;
	}
      }
      break;
      default:
	flag |= MUTT_PDR_ERRORDONE;
    }
    SKIPWS (pc);
  }
  if ((flag & MUTT_PDR_ERROR) && !(flag & MUTT_PDR_ABSOLUTE))
  { /* getDate has its own error message, don't overwrite it here */
    snprintf (err->data, err->dsize, _("Invalid relative date: %s"), pc-1);
  }
  return ((flag & MUTT_PDR_ERROR) ? NULL : pc);
}

static int eat_date (pattern_t *pat, BUFFER *s, BUFFER *err)
{
  BUFFER buffer;
  struct tm min, max;
  char *pexpr;

  mutt_buffer_init (&buffer);
  pexpr = s->dptr;
  if (mutt_extract_token (&buffer, s, MUTT_TOKEN_COMMENT | MUTT_TOKEN_PATTERN) != 0
      || !buffer.data)
  {
    snprintf (err->data, err->dsize, _("Error in expression: %s"), pexpr);
    return (-1);
  }
  if (!*buffer.data)
  {
    snprintf (err->data, err->dsize, _("Empty expression"));
    return (-1);
  }

  memset (&min, 0, sizeof (min));
  /* the `0' time is Jan 1, 1970 UTC, so in order to prevent a negative time
     when doing timezone conversion, we use Jan 2, 1970 UTC as the base
     here */
  min.tm_mday = 2;
  min.tm_year = 70;

  memset (&max, 0, sizeof (max));

  /* Arbitrary year in the future.  Don't set this too high
     or mutt_mktime() returns something larger than will
     fit in a time_t on some systems */
  max.tm_year = 130;
  max.tm_mon = 11;
  max.tm_mday = 31;
  max.tm_hour = 23;
  max.tm_min = 59;
  max.tm_sec = 59;

  if (strchr ("<>=", buffer.data[0]))
  {
    /* offset from current time
       <3d	less than three days ago
       >3d	more than three days ago
       =3d	exactly three days ago */
    time_t now = time (NULL);
    struct tm *tm = localtime (&now);
    int exact = 0;

    if (buffer.data[0] == '<')
    {
      memcpy (&min, tm, sizeof (min));
      tm = &min;
    }
    else
    {
      memcpy (&max, tm, sizeof (max));
      tm = &max;

      if (buffer.data[0] == '=')
	exact++;
    }
    tm->tm_hour = 23;
    tm->tm_min = tm->tm_sec = 59;

    /* force negative offset */
    get_offset (tm, buffer.data + 1, -1);

    if (exact)
    {
      /* start at the beginning of the day in question */
      memcpy (&min, &max, sizeof (max));
      min.tm_hour = min.tm_sec = min.tm_min = 0;
    }
  }
  else
  {
    const char *pc = buffer.data;

    int haveMin = FALSE;
    int untilNow = FALSE;
    if (isdigit ((unsigned char)*pc))
    {
      /* minimum date specified */
      if ((pc = getDate (pc, &min, err)) == NULL)
      {
	FREE (&buffer.data);
	return (-1);
      }
      haveMin = TRUE;
      SKIPWS (pc);
      if (*pc == '-')
      {
        const char *pt = pc + 1;
	SKIPWS (pt);
	untilNow = (*pt == '\0');
      }
    }

    if (!untilNow)
    { /* max date or relative range/window */

      struct tm baseMin;

      if (!haveMin)
      { /* save base minimum and set current date, e.g. for "-3d+1d" */
	time_t now = time (NULL);
	struct tm *tm = localtime (&now);
	memcpy (&baseMin, &min, sizeof(baseMin));
	memcpy (&min, tm, sizeof (min));
	min.tm_hour = min.tm_sec = min.tm_min = 0;
      }
      
      /* preset max date for relative offsets,
	 if nothing follows we search for messages on a specific day */
      max.tm_year = min.tm_year;
      max.tm_mon = min.tm_mon;
      max.tm_mday = min.tm_mday;

      if (!parse_date_range (pc, &min, &max, haveMin, &baseMin, err))
      { /* bail out on any parsing error */
	FREE (&buffer.data);
	return (-1);
      }
    }
  }

  /* Since we allow two dates to be specified we'll have to adjust that. */
  adjust_date_range (&min, &max);

  pat->min = mutt_mktime (&min, 1);
  pat->max = mutt_mktime (&max, 1);

  FREE (&buffer.data);

  return 0;
}

static int patmatch (const pattern_t* pat, const char* buf)
{
  if (pat->stringmatch)
    return pat->ign_case ? !strcasestr (buf, pat->p.str) :
			   !strstr (buf, pat->p.str);
  else if (pat->groupmatch)
    return !mutt_group_match (pat->p.g, buf);
  else
    return regexec (pat->p.rx, buf, 0, NULL, 0);
}

static const struct pattern_flags *lookup_tag (char tag)
{
  int i;

  for (i = 0; Flags[i].tag; i++)
    if (Flags[i].tag == tag)
      return (&Flags[i]);
  return NULL;
}

static /* const */ char *find_matching_paren (/* const */ char *s)
{
  int level = 1;

  for (; *s; s++)
  {
    if (*s == '(')
      level++;
    else if (*s == ')')
    {
      level--;
      if (!level)
	break;
    }
  }
  return s;
}

void mutt_pattern_free (pattern_t **pat)
{
  pattern_t *tmp;

  while (*pat)
  {
    tmp = *pat;
    *pat = (*pat)->next;

    if (tmp->stringmatch)
      FREE (&tmp->p.str);
    else if (tmp->groupmatch)
      tmp->p.g = NULL;
    else if (tmp->p.rx)
    {
      regfree (tmp->p.rx);
      FREE (&tmp->p.rx);
    }

    if (tmp->child)
      mutt_pattern_free (&tmp->child);
    FREE (&tmp);
  }
}

pattern_t *mutt_pattern_comp (/* const */ char *s, int flags, BUFFER *err)
{
  pattern_t *curlist = NULL;
  pattern_t *tmp, *tmp2;
  pattern_t *last = NULL;
  int not = 0;
  int alladdr = 0;
  int or = 0;
  int implicit = 1;	/* used to detect logical AND operator */
  int isalias = 0;
  const struct pattern_flags *entry;
  char *p;
  char *buf;
  BUFFER ps;

  mutt_buffer_init (&ps);
  ps.dptr = s;
  ps.dsize = mutt_strlen (s);

  while (*ps.dptr)
  {
    SKIPWS (ps.dptr);
    switch (*ps.dptr)
    {
      case '^':
	ps.dptr++;
	alladdr = !alladdr;
	break;
      case '!':
	ps.dptr++;
	not = !not;
	break;
      case '@':
	ps.dptr++;
	isalias = !isalias;
	break;
      case '|':
	if (!or)
	{
	  if (!curlist)
	  {
	    snprintf (err->data, err->dsize, _("error in pattern at: %s"), ps.dptr);
	    return NULL;
	  }
	  if (curlist->next)
	  {
	    /* A & B | C == (A & B) | C */
	    tmp = new_pattern ();
	    tmp->op = MUTT_AND;
	    tmp->child = curlist;

	    curlist = tmp;
	    last = curlist;
	  }

	  or = 1;
	}
	ps.dptr++;
	implicit = 0;
	not = 0;
	alladdr = 0;
	isalias = 0;
	break;
      case '%':
      case '=':
      case '~':
	if (!*(ps.dptr + 1))
	{
	  snprintf (err->data, err->dsize, _("missing pattern: %s"), ps.dptr);
	  mutt_pattern_free (&curlist);
	  return NULL;
	}
	if (*(ps.dptr + 1) == '(')
        {
	  ps.dptr ++; /* skip ~ */
	  p = find_matching_paren (ps.dptr + 1);
	  if (*p != ')')
	  {
	    snprintf (err->data, err->dsize, _("mismatched brackets: %s"), ps.dptr);
	    mutt_pattern_free (&curlist);
	    return NULL;
	  }
	  tmp = new_pattern ();
	  tmp->op = MUTT_THREAD;
	  if (last)
	    last->next = tmp;
	  else
	    curlist = tmp;
	  last = tmp;
	  tmp->not ^= not;
	  tmp->alladdr |= alladdr;
	  tmp->isalias |= isalias;
	  not = 0;
	  alladdr = 0;
	  isalias = 0;
	  /* compile the sub-expression */
	  buf = mutt_substrdup (ps.dptr + 1, p);
	  if ((tmp2 = mutt_pattern_comp (buf, flags, err)) == NULL)
	  {
	    FREE (&buf);
	    mutt_pattern_free (&curlist);
	    return NULL;
	  }
	  FREE (&buf);
	  tmp->child = tmp2;
	  ps.dptr = p + 1; /* restore location */
	  break;
	}
        if (implicit && or)
	{
	  /* A | B & C == (A | B) & C */
	  tmp = new_pattern ();
	  tmp->op = MUTT_OR;
	  tmp->child = curlist;
	  curlist = tmp;
	  last = tmp;
	  or = 0;
	}

	tmp = new_pattern ();
	tmp->not = not;
	tmp->alladdr = alladdr;
	tmp->isalias = isalias;
        tmp->stringmatch = (*ps.dptr == '=') ? 1 : 0;
        tmp->groupmatch  = (*ps.dptr == '%') ? 1 : 0;
	not = 0;
	alladdr = 0;
	isalias = 0;

	if (last)
	  last->next = tmp;
	else
	  curlist = tmp;
	last = tmp;

	ps.dptr++; /* move past the ~ */
	if ((entry = lookup_tag (*ps.dptr)) == NULL)
	{
	  snprintf (err->data, err->dsize, _("%c: invalid pattern modifier"), *ps.dptr);
	  mutt_pattern_free (&curlist);
	  return NULL;
	}
	if (entry->class && (flags & entry->class) == 0)
	{
	  snprintf (err->data, err->dsize, _("%c: not supported in this mode"), *ps.dptr);
	  mutt_pattern_free (&curlist);
	  return NULL;
	}
	tmp->op = entry->op;

	ps.dptr++; /* eat the operator and any optional whitespace */
	SKIPWS (ps.dptr);

	if (entry->eat_arg)
	{
	  if (!*ps.dptr)
	  {
	    snprintf (err->data, err->dsize, _("missing parameter"));
	    mutt_pattern_free (&curlist);
	    return NULL;
	  }
	  if (entry->eat_arg (tmp, &ps, err) == -1)
	  {
	    mutt_pattern_free (&curlist);
	    return NULL;
	  }
	}
	implicit = 1;
	break;
      case '(':
	p = find_matching_paren (ps.dptr + 1);
	if (*p != ')')
	{
	  snprintf (err->data, err->dsize, _("mismatched parenthesis: %s"), ps.dptr);
	  mutt_pattern_free (&curlist);
	  return NULL;
	}
	/* compile the sub-expression */
	buf = mutt_substrdup (ps.dptr + 1, p);
	if ((tmp = mutt_pattern_comp (buf, flags, err)) == NULL)
	{
	  FREE (&buf);
	  mutt_pattern_free (&curlist);
	  return NULL;
	}
	FREE (&buf);
	if (last)
	  last->next = tmp;
	else
	  curlist = tmp;
	last = tmp;
	tmp->not ^= not;
	tmp->alladdr |= alladdr;
	tmp->isalias |= isalias;
	not = 0;
	alladdr = 0;
	isalias = 0;
	ps.dptr = p + 1; /* restore location */
	break;
      default:
	snprintf (err->data, err->dsize, _("error in pattern at: %s"), ps.dptr);
	mutt_pattern_free (&curlist);
	return NULL;
    }
  }
  if (!curlist)
  {
    strfcpy (err->data, _("empty pattern"), err->dsize);
    return NULL;
  }
  if (curlist->next)
  {
    tmp = new_pattern ();
    tmp->op = or ? MUTT_OR : MUTT_AND;
    tmp->child = curlist;
    curlist = tmp;
  }
  return (curlist);
}

static int
perform_and (pattern_t *pat, pattern_exec_flag flags, CONTEXT *ctx, HEADER *hdr)
{
  for (; pat; pat = pat->next)
    if (mutt_pattern_exec (pat, flags, ctx, hdr) <= 0)
      return 0;
  return 1;
}

static int
perform_or (struct pattern_t *pat, pattern_exec_flag flags, CONTEXT *ctx, HEADER *hdr)
{
  for (; pat; pat = pat->next)
    if (mutt_pattern_exec (pat, flags, ctx, hdr) > 0)
      return 1;
  return 0;
}

static int match_adrlist (pattern_t *pat, int match_personal, int n, ...)
{
  va_list ap;
  ADDRESS *a;

  va_start (ap, n);
  for ( ; n ; n --)
  {
    for (a = va_arg (ap, ADDRESS *) ; a ; a = a->next)
    {
      if (pat->alladdr ^
          ((!pat->isalias || alias_reverse_lookup (a)) &&
           ((a->mailbox && !patmatch (pat, a->mailbox)) ||
	    (match_personal && a->personal && !patmatch (pat, a->personal) ))))
      {
	va_end (ap);
	return (! pat->alladdr); /* Found match, or non-match if alladdr */
      }
    }
  }
  va_end (ap);
  return pat->alladdr; /* No matches, or all matches if alladdr */
}

static int match_reference (pattern_t *pat, LIST *refs)
{
  for (; refs; refs = refs->next)
    if (patmatch (pat, refs->data) == 0)
      return 1;
  return 0;
}

/*
 * Matches subscribed mailing lists
 */
int mutt_is_list_recipient (int alladdr, ADDRESS *a1, ADDRESS *a2)
{
  for (; a1 ; a1 = a1->next)
    if (alladdr ^ mutt_is_subscribed_list (a1))
      return (! alladdr);
  for (; a2 ; a2 = a2->next)
    if (alladdr ^ mutt_is_subscribed_list (a2))
      return (! alladdr);
  return alladdr;
}

/*
 * Matches known mailing lists
 * The function name may seem a little bit misleading: It checks all
 * recipients in To and Cc for known mailing lists, subscribed or not.
 */
int mutt_is_list_cc (int alladdr, ADDRESS *a1, ADDRESS *a2)
{
  for (; a1 ; a1 = a1->next)
    if (alladdr ^ mutt_is_mail_list (a1))
      return (! alladdr);
  for (; a2 ; a2 = a2->next)
    if (alladdr ^ mutt_is_mail_list (a2))
      return (! alladdr);
  return alladdr;
}

static int match_user (int alladdr, ADDRESS *a1, ADDRESS *a2)
{
  for (; a1 ; a1 = a1->next)
    if (alladdr ^ mutt_addr_is_user (a1))
      return (! alladdr);
  for (; a2 ; a2 = a2->next)
    if (alladdr ^ mutt_addr_is_user (a2))
      return (! alladdr);
  return alladdr;
}

static int match_threadcomplete(struct pattern_t *pat, pattern_exec_flag flags, CONTEXT *ctx, THREAD *t,int left,int up,int right,int down)
{
  int a;
  HEADER *h;

  if(!t)
    return 0;
  h = t->message;
  if(h)
    if(mutt_pattern_exec(pat, flags, ctx, h))
      return 1;

  if(up && (a=match_threadcomplete(pat, flags, ctx, t->parent,1,1,1,0)))
    return a;
  if(right && t->parent && (a=match_threadcomplete(pat, flags, ctx, t->next,0,0,1,1)))
    return a;
  if(left && t->parent && (a=match_threadcomplete(pat, flags, ctx, t->prev,1,0,0,1)))
    return a;
  if(down && (a=match_threadcomplete(pat, flags, ctx, t->child,1,0,1,1)))
    return a;
  return 0;
}

/* flags
   	MUTT_MATCH_FULL_ADDRESS	match both personal and machine address */
int
mutt_pattern_exec (struct pattern_t *pat, pattern_exec_flag flags, CONTEXT *ctx, HEADER *h)
{
  switch (pat->op)
  {
    case MUTT_AND:
      return (pat->not ^ (perform_and (pat->child, flags, ctx, h) > 0));
    case MUTT_OR:
      return (pat->not ^ (perform_or (pat->child, flags, ctx, h) > 0));
    case MUTT_THREAD:
      return (pat->not ^ match_threadcomplete(pat->child, flags, ctx, h->thread, 1, 1, 1, 1));
    case MUTT_ALL:
      return (!pat->not);
    case MUTT_EXPIRED:
      return (pat->not ^ h->expired);
    case MUTT_SUPERSEDED:
      return (pat->not ^ h->superseded);
    case MUTT_FLAG:
      return (pat->not ^ h->flagged);
    case MUTT_TAG:
      return (pat->not ^ h->tagged);
    case MUTT_NEW:
      return (pat->not ? h->old || h->read : !(h->old || h->read));
    case MUTT_UNREAD:
      return (pat->not ? h->read : !h->read);
    case MUTT_REPLIED:
      return (pat->not ^ h->replied);
    case MUTT_OLD:
      return (pat->not ? (!h->old || h->read) : (h->old && !h->read));
    case MUTT_READ:
      return (pat->not ^ h->read);
    case MUTT_DELETED:
      return (pat->not ^ h->deleted);
    case MUTT_MESSAGE:
      return (pat->not ^ (h->msgno >= pat->min - 1 && (pat->max == MUTT_MAXRANGE ||
						   h->msgno <= pat->max - 1)));
    case MUTT_DATE:
      return (pat->not ^ (h->date_sent >= pat->min && h->date_sent <= pat->max));
    case MUTT_DATE_RECEIVED:
      return (pat->not ^ (h->received >= pat->min && h->received <= pat->max));
    case MUTT_BODY:
    case MUTT_HEADER:
    case MUTT_WHOLE_MSG:
      /*
       * ctx can be NULL in certain cases, such as when replying to a message from the attachment menu and
       * the user has a reply-hook using "~h" (bug #2190).
       * This is also the case when message scoring.
       */
      if (!ctx)
	      return 0;
#ifdef USE_IMAP
      /* IMAP search sets h->matched at search compile time */
      if (ctx->magic == MUTT_IMAP && pat->stringmatch)
	return (h->matched);
#endif
      return (pat->not ^ msg_search (ctx, pat, h->msgno));
    case MUTT_SENDER:
      return (pat->not ^ match_adrlist (pat, flags & MUTT_MATCH_FULL_ADDRESS, 1,
                                        h->env->sender));
    case MUTT_FROM:
      return (pat->not ^ match_adrlist (pat, flags & MUTT_MATCH_FULL_ADDRESS, 1,
                                        h->env->from));
    case MUTT_TO:
      return (pat->not ^ match_adrlist (pat, flags & MUTT_MATCH_FULL_ADDRESS, 1,
                                        h->env->to));
    case MUTT_CC:
      return (pat->not ^ match_adrlist (pat, flags & MUTT_MATCH_FULL_ADDRESS, 1,
                                        h->env->cc));
    case MUTT_SUBJECT:
      return (pat->not ^ (h->env->subject && patmatch (pat, h->env->subject) == 0));
    case MUTT_ID:
      return (pat->not ^ (h->env->message_id && patmatch (pat, h->env->message_id) == 0));
    case MUTT_SCORE:
      return (pat->not ^ (h->score >= pat->min && (pat->max == MUTT_MAXRANGE ||
						   h->score <= pat->max)));
    case MUTT_SIZE:
      return (pat->not ^ (h->content->length >= pat->min && (pat->max == MUTT_MAXRANGE || h->content->length <= pat->max)));
    case MUTT_REFERENCE:
      return (pat->not ^ (match_reference (pat, h->env->references) ||
			  match_reference (pat, h->env->in_reply_to)));
    case MUTT_ADDRESS:
      return (pat->not ^ match_adrlist (pat, flags & MUTT_MATCH_FULL_ADDRESS, 4,
                                        h->env->from, h->env->sender,
                                        h->env->to, h->env->cc));
    case MUTT_RECIPIENT:
           return (pat->not ^ match_adrlist (pat, flags & MUTT_MATCH_FULL_ADDRESS,
                                             2, h->env->to, h->env->cc));
    case MUTT_LIST:	/* known list, subscribed or not */
      return (pat->not ^ mutt_is_list_cc (pat->alladdr, h->env->to, h->env->cc));
    case MUTT_SUBSCRIBED_LIST:
      return (pat->not ^ mutt_is_list_recipient (pat->alladdr, h->env->to, h->env->cc));
    case MUTT_PERSONAL_RECIP:
      return (pat->not ^ match_user (pat->alladdr, h->env->to, h->env->cc));
    case MUTT_PERSONAL_FROM:
      return (pat->not ^ match_user (pat->alladdr, h->env->from, NULL));
    case MUTT_COLLAPSED:
      return (pat->not ^ (h->collapsed && h->num_hidden > 1));
   case MUTT_CRYPT_SIGN:
     if (!WithCrypto)
       break;
     return (pat->not ^ ((h->security & SIGN) ? 1 : 0));
   case MUTT_CRYPT_VERIFIED:
     if (!WithCrypto)
       break;
     return (pat->not ^ ((h->security & GOODSIGN) ? 1 : 0));
   case MUTT_CRYPT_ENCRYPT:
     if (!WithCrypto)
       break;
     return (pat->not ^ ((h->security & ENCRYPT) ? 1 : 0));
   case MUTT_PGP_KEY:
     if (!(WithCrypto & APPLICATION_PGP))
       break;
     return (pat->not ^ ((h->security & APPLICATION_PGP) && (h->security & PGPKEY)));
    case MUTT_XLABEL:
      {
        LIST *label;
        int result = 0;
        for (label = h->env->labels; label; label = label->next)
        {
          if (label->data == NULL)
            continue;
          result = patmatch (pat, label->data) == 0;
          if (result)
            break;
        }
        return pat->not ^ result;
      }
      return (pat->not ^ (h->env->x_label && patmatch (pat, h->env->x_label) == 0));
#ifdef USE_NOTMUCH
    case MUTT_NOTMUCH_LABEL:
      {
      char *tags = nm_header_get_tags(h);
      return (pat->not ^ (tags && patmatch (pat, tags) == 0));
      }
#endif
    case MUTT_HORMEL:
      return (pat->not ^ (h->env->spam && h->env->spam->data && patmatch (pat, h->env->spam->data) == 0));
    case MUTT_DUPLICATED:
      return (pat->not ^ (h->thread && h->thread->duplicate_thread));
    case MUTT_MIMEATTACH:
      if (!ctx)
        return 0;
      {
      int count = mutt_count_body_parts (ctx, h);
      return (pat->not ^ (count >= pat->min && (pat->max == MUTT_MAXRANGE ||
                                                count <= pat->max)));
      }
    case MUTT_UNREFERENCED:
      return (pat->not ^ (h->thread && !h->thread->child));
#ifdef USE_NNTP
    case MUTT_NEWSGROUPS:
      return (pat->not ^ (h->env->newsgroups && patmatch (pat, h->env->newsgroups) == 0));
#endif
  }
  mutt_error (_("error: unknown op %d (report this error)."), pat->op);
  return (-1);
}

static void quote_simple(char *tmp, size_t len, const char *p)
{
  int i = 0;
  
  tmp[i++] = '"';
  while (*p && i < len - 3)
  {
    if (*p == '\\' || *p == '"')
      tmp[i++] = '\\';
    tmp[i++] = *p++;
  }
  tmp[i++] = '"';
  tmp[i] = 0;
}
  
/* convert a simple search into a real request */
void mutt_check_simple (char *s, size_t len, const char *simple)
{
  char tmp[LONG_STRING];
  int do_simple = 1;
  char *p;

  for (p = s; p && *p; p++)
  {
    if (*p == '\\' && *(p + 1))
      p++;
    else if (*p == '~' || *p == '=' || *p == '%')
    {
      do_simple = 0;
      break;
    }
  }

  /* XXX - is ascii_strcasecmp() right here, or should we use locale's
   * equivalences?
   */

  if (do_simple) /* yup, so spoof a real request */
  {
    /* convert old tokens into the new format */
    if (ascii_strcasecmp ("all", s) == 0 ||
	!mutt_strcmp ("^", s) || !mutt_strcmp (".", s)) /* ~A is more efficient */
      strfcpy (s, "~A", len);
    else if (ascii_strcasecmp ("del", s) == 0)
      strfcpy (s, "~D", len);
    else if (ascii_strcasecmp ("flag", s) == 0)
      strfcpy (s, "~F", len);
    else if (ascii_strcasecmp ("new", s) == 0)
      strfcpy (s, "~N", len);
    else if (ascii_strcasecmp ("old", s) == 0)
      strfcpy (s, "~O", len);
    else if (ascii_strcasecmp ("repl", s) == 0)
      strfcpy (s, "~Q", len);
    else if (ascii_strcasecmp ("read", s) == 0)
      strfcpy (s, "~R", len);
    else if (ascii_strcasecmp ("tag", s) == 0)
      strfcpy (s, "~T", len);
    else if (ascii_strcasecmp ("unread", s) == 0)
      strfcpy (s, "~U", len);
    else
    {
      quote_simple (tmp, sizeof(tmp), s);
      mutt_expand_fmt (s, len, simple, tmp);
    }
  }
}

/**
 * top_of_thread - Find the first email in the current thread
 * @h: Header of current email
 *
 * Returns:
 *  THREAD*: success, email found
 *  NULL:    on error
 */
static THREAD *
top_of_thread (HEADER *h)
{
  THREAD *t;

  if (!h)
    return NULL;

  t = h->thread;

  while (t && t->parent)
    t = t->parent;

  return t;
}

/**
 * mutt_limit_current_thread - Limit the email view to the current thread
 * @h: Header of current email
 *
 * Returns:
 *  1: Success
 *  0: Failure
 */
int
mutt_limit_current_thread (HEADER *h)
{
  int i;
  THREAD *me;

  if (!h)
    return 0;

  me = top_of_thread (h);
  if (!me)
    return 0;

  Context->vcount    = 0;
  Context->vsize     = 0;
  Context->collapsed = 0;

  for (i = 0; i < Context->msgcount; i++)
  {
    Context->hdrs[i]->virtual    = -1;
    Context->hdrs[i]->limited    = 0;
    Context->hdrs[i]->collapsed  = 0;
    Context->hdrs[i]->num_hidden = 0;

    if (top_of_thread (Context->hdrs[i]) == me)
    {
      BODY *body = Context->hdrs[i]->content;

      Context->hdrs[i]->virtual = Context->vcount;
      Context->hdrs[i]->limited = 1;
      Context->v2r[Context->vcount] = i;
      Context->vcount++;
      Context->vsize += (body->length + body->offset - body->hdr_offset);
    }
  }
  return 1;
}

int mutt_pattern_func (int op, char *prompt)
{
  pattern_t *pat;
  char buf[LONG_STRING] = "", *simple;
  BUFFER err;
  int i;
  progress_t progress;

  strfcpy (buf, NONULL (Context->pattern), sizeof (buf));
  if (prompt || op != MUTT_LIMIT)
  if (mutt_get_field (prompt, buf, sizeof (buf), MUTT_PATTERN | MUTT_CLEAR) != 0 || !buf[0])
    return (-1);

  mutt_message _("Compiling search pattern...");
  
  simple = safe_strdup (buf);
  mutt_check_simple (buf, sizeof (buf), NONULL (SimpleSearch));

  mutt_buffer_init (&err);
  err.dsize = STRING;
  err.data = safe_malloc(err.dsize);
  if ((pat = mutt_pattern_comp (buf, MUTT_FULL_MSG, &err)) == NULL)
  {
    FREE (&simple);
    mutt_error ("%s", err.data);
    FREE (&err.data);
    return (-1);
  }

#ifdef USE_IMAP
  if (Context->magic == MUTT_IMAP && imap_search (Context, pat) < 0)
    return -1;
#endif

  mutt_progress_init (&progress, _("Executing command on matching messages..."),
		      MUTT_PROGRESS_MSG, ReadInc,
		      (op == MUTT_LIMIT) ? Context->msgcount : Context->vcount);

#define THIS_BODY Context->hdrs[i]->content

  if (op == MUTT_LIMIT)
  {
    Context->vcount    = 0;
    Context->vsize     = 0;
    Context->collapsed = 0;

    for (i = 0; i < Context->msgcount; i++)
    {
      mutt_progress_update (&progress, i, -1);
      /* new limit pattern implicitly uncollapses all threads */
      Context->hdrs[i]->virtual = -1;
      Context->hdrs[i]->limited = 0;
      Context->hdrs[i]->collapsed = 0;
      Context->hdrs[i]->num_hidden = 0;
      if (mutt_pattern_exec (pat, MUTT_MATCH_FULL_ADDRESS, Context, Context->hdrs[i]))
      {
	Context->hdrs[i]->virtual = Context->vcount;
	Context->hdrs[i]->limited = 1;
	Context->v2r[Context->vcount] = i;
	Context->vcount++;
	Context->vsize+=THIS_BODY->length + THIS_BODY->offset -
	  THIS_BODY->hdr_offset;
      }
    }
  }
  else
  {
    for (i = 0; i < Context->vcount; i++)
    {
      mutt_progress_update (&progress, i, -1);
      if (mutt_pattern_exec (pat, MUTT_MATCH_FULL_ADDRESS, Context, Context->hdrs[Context->v2r[i]]))
      {
	switch (op)
	{
          case MUTT_UNDELETE:
            mutt_set_flag (Context, Context->hdrs[Context->v2r[i]], MUTT_PURGE,
                           0);
	  case MUTT_DELETE:
	    mutt_set_flag (Context, Context->hdrs[Context->v2r[i]], MUTT_DELETE, 
			  (op == MUTT_DELETE));
	    break;
	  case MUTT_TAG:
	  case MUTT_UNTAG:
	    mutt_set_flag (Context, Context->hdrs[Context->v2r[i]], MUTT_TAG, 
			   (op == MUTT_TAG));
	    break;
	}
      }
    }
  }

#undef THIS_BODY

  mutt_clear_error ();

  if (op == MUTT_LIMIT)
  {
    /* drop previous limit pattern */
    FREE (&Context->pattern);
    if (Context->limit_pattern)
      mutt_pattern_free (&Context->limit_pattern);

    if (Context->msgcount && !Context->vcount)
      mutt_error _("No messages matched criteria.");

    /* record new limit pattern, unless match all */
    if (mutt_strcmp (buf, "~A") != 0)
    {
      Context->pattern = simple;
      simple = NULL; /* don't clobber it */
      Context->limit_pattern = mutt_pattern_comp (buf, MUTT_FULL_MSG, &err);
    }
  }
  FREE (&simple);
  mutt_pattern_free (&pat);
  FREE (&err.data);

  return 0;
}

int mutt_search_command (int cur, int op)
{
  int i, j;
  char buf[STRING];
  char temp[LONG_STRING];
  int incr;
  HEADER *h;
  progress_t progress;
  const char* msg = NULL;

  if (!*LastSearch || (op != OP_SEARCH_NEXT && op != OP_SEARCH_OPPOSITE))
  {
    strfcpy (buf, *LastSearch ? LastSearch : "", sizeof (buf));
    if (mutt_get_field ((op == OP_SEARCH || op == OP_SEARCH_NEXT) ?
			_("Search for: ") : _("Reverse search for: "),
			buf, sizeof (buf),
		      MUTT_CLEAR | MUTT_PATTERN) != 0 || !buf[0])
      return (-1);

    if (op == OP_SEARCH || op == OP_SEARCH_NEXT)
      unset_option (OPTSEARCHREVERSE);
    else
      set_option (OPTSEARCHREVERSE);

    /* compare the *expanded* version of the search pattern in case 
       $simple_search has changed while we were searching */
    strfcpy (temp, buf, sizeof (temp));
    mutt_check_simple (temp, sizeof (temp), NONULL (SimpleSearch));

    if (!SearchPattern || mutt_strcmp (temp, LastSearchExpn))
    {
      BUFFER err;
      mutt_buffer_init (&err);
      set_option (OPTSEARCHINVALID);
      strfcpy (LastSearch, buf, sizeof (LastSearch));
      mutt_message _("Compiling search pattern...");
      mutt_pattern_free (&SearchPattern);
      err.dsize = STRING;
      err.data = safe_malloc (err.dsize);
      if ((SearchPattern = mutt_pattern_comp (temp, MUTT_FULL_MSG, &err)) == NULL)
      {
	mutt_error ("%s", err.data);
	FREE (&err.data);
	LastSearch[0] = '\0';
	return (-1);
      }
      mutt_clear_error ();
    }
  }

  if (option (OPTSEARCHINVALID))
  {
    for (i = 0; i < Context->msgcount; i++)
      Context->hdrs[i]->searched = 0;
#ifdef USE_IMAP
    if (Context->magic == MUTT_IMAP && imap_search (Context, SearchPattern) < 0)
      return -1;
#endif
    unset_option (OPTSEARCHINVALID);
  }

  incr = (option (OPTSEARCHREVERSE)) ? -1 : 1;
  if (op == OP_SEARCH_OPPOSITE)
    incr = -incr;

  mutt_progress_init (&progress, _("Searching..."), MUTT_PROGRESS_MSG,
		      ReadInc, Context->vcount);

  for (i = cur + incr, j = 0 ; j != Context->vcount; j++)
  {
    mutt_progress_update (&progress, j, -1);
    if (i > Context->vcount - 1)
    {
      i = 0;
      if (option (OPTWRAPSEARCH))
        msg = _("Search wrapped to top.");
      else 
      {
        mutt_message _("Search hit bottom without finding match");
	return (-1);
      }
    }
    else if (i < 0)
    {
      i = Context->vcount - 1;
      if (option (OPTWRAPSEARCH))
        msg = _("Search wrapped to bottom.");
      else 
      {
        mutt_message _("Search hit top without finding match");
	return (-1);
      }
    }

    h = Context->hdrs[Context->v2r[i]];
    if (h->searched)
    {
      /* if we've already evaluated this message, use the cached value */
      if (h->matched)
      {
	mutt_clear_error();
	if (msg && *msg)
	  mutt_message (msg);
	return i;
      }
    }
    else
    {
      /* remember that we've already searched this message */
      h->searched = 1;
      if ((h->matched = (mutt_pattern_exec (SearchPattern, MUTT_MATCH_FULL_ADDRESS, Context, h) > 0)))
      {
	mutt_clear_error();
	if (msg && *msg)
	  mutt_message (msg);
	return i;
      }
    }

    if (SigInt)
    {
      mutt_error _("Search interrupted.");
      SigInt = 0;
      return (-1);
    }

    i += incr;
  }

  mutt_error _("Not found.");
  return (-1);
}
