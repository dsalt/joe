/*
 *	Key-map handler
 *	Copyright
 *		(C) 1992 Joseph H. Allen
 *
 *	This file is part of JOE (Joe's Own Editor)
 */
#include "types.h"
#define DEBUG 1

struct context *contexts; /* Global list of KMAPs */
char **keymap_list; /* KMAP names array for completion */

/* Nobody's going to add more bits, right? */
#define MOD_SHIFT	1
#define MOD_ALT		2
#define MOD_CTRL	4
#define MOD_SUPER	8	/* Apparently, this one's Meta */
#define MOD_HYPER	16
#define MOD_META	32	/* which makes this... what? */
#define MOD_CAPS_LOCK	64
#define MOD_NUM_LOCK	128

#define MOD_KEYS        7
#define MOD_KEYS_EXT    56
#define MOD_KEYS_ALL    63
#define MOD_LOCKS	192


/* Input escape sequence handling (particularly CSI-u)
 * See https://sw.kovidgoyal.net/kitty/keyboard-protocol/ for codes
 *
 * CSI arg0 : arg1 ; arg2 : arg3 ; codepoints u
 *   arg0: unicode key code
 *   arg1: base key key code ('A' for 'a', '1' for '!', etc.)
 *   arg2: extended modifiers (see below)
 *   arg3: 1 = press, 2 = repeat, 3 = release
 *   codepoints: colon-separated list of Unicode values (note: arg0 = 0)
 *
 * With CSI-u mode 0x01, we'll just get arg0 and arg2.
 * But parse it all anyway. */

/* arg[n] or u.<foo> */
#define IS_DEFAULT(v) ((v) == -1 || (v) == 1)
#define ARG_DEFAULT(v) ((v) == -1 ? 1 : (v))
#define CHR_DEFAULT(arg) ((arg) <= 0 ? ' ' : (arg))
/* 0 : [1] : [2] ; 3 : [4] ; [5] : ... */
/* array is {:,;} so that (char & 1) works */
static const unsigned char nextarg[ARGS_MAX][2] = {
	{1,3}, { 2,3}, { 0,3}, { 4,5}, { 5,5}, { 6,0}, { 7,0}, { 8,0},
	{9,0}, {10,0}, {11,0}, {12,0}, {13,0}, {14,0}, {15,0}
};

/* For testing modifier key bits. Always arg 2. */
#define MOD_MASK(esc, mask) (esc->u.mod > 0 ? (esc->u.mod - 1) & (mask) : 0)
#define MOD_ARG(esc) (MOD_MASK((esc), MOD_KEYS_ALL) + 1)

/* Print, Pause, Menu & function keys (F1-F35)
 * F1-F12 not present, but space is reserved */
#define PRINT_PAUSE_MENU_BASE 57361
#define FUNCTION_KEYS_BASE 57364
#define FUNCTION_KEYS_LAST 57398
/* Keypad digits, symbols, Enter */
#define KEYPAD_KEYS_BASE 57399
#define KEYPAD_KEYS_LAST 57416
#if CSI_U_LEVEL >= 1
static const int keypad_keys[] = {
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
	'.', '/', '*', '-', '+', '\r', '=', ','
};
#endif
/* Keypad funcs from 57417, media keys from 57428,
 * modifier keys from 57441, last one at 57454
 * Here, we want keypad funcs & media keys */
#define KEYPAD_FUNCS_BASE 57417
#define KEYPAD_FUNCS_LAST 57440
/* Modifier keys */
#define KEY_MODS_BASE 57441
#define KEY_MODS_LAST 57454

#ifdef DEBUG
static void LOGESCSEQ(const struct esc_parse *esc)
{
	char *p = msgbuf;
	char *const e = msgbuf + JOE_MSGBUFSIZE;
	int i;
	/* not logging args, despite Konsole */
	joe_snprintf_0(p, e-p, "esc  : ESC");
	for (i = -1; i < esc->end; ++i) {
		p += strlen(p);
		joe_snprintf_1(p, e-p, " %lc", esc->buf[i]);
	}
	p += strlen(p);
	joe_snprintf_0(p, e-p, "\n");
	logmessage_0(msgbuf);
	msgbuf[strlen(msgbuf)-1] = 0;
	msgnw(maint->curwin, msgbuf);
}
static void LOGCSIU(const struct esc_parse *esc, const char *label)
{
	size_t s;
	joe_snprintf_6(msgbuf, JOE_MSGBUFSIZE, "%s: CSI %d:%d:%d;%d:%d", label, ARG_DEFAULT(esc->u.key), ARG_DEFAULT(esc->u.shifted), ARG_DEFAULT(esc->u.base), ARG_DEFAULT(esc->u.mod), ARG_DEFAULT(esc->u.event)); \
	for (int i = 0; i < ARG_CP_MAX && esc->u.cp[i] != -1; ++i) {
		s = strlen(msgbuf);
		joe_snprintf_1(msgbuf + s, JOE_MSGBUFSIZE - s, ":%d", esc->u.cp[i]);
	}
	s = strlen(msgbuf);
	joe_snprintf_6(msgbuf + s, JOE_MSGBUFSIZE - s, " %lc %lc %lc %c%c%c\n",
	               CHR_DEFAULT(esc->u.key), CHR_DEFAULT(esc->u.shifted), CHR_DEFAULT(esc->u.base),
	               MOD_MASK(esc, MOD_SHIFT)?'S':' ', MOD_MASK(esc, MOD_CTRL)?'C':' ', MOD_MASK(esc, MOD_ALT)?'M':' ');
	logmessage_0(msgbuf);
	msgbuf[strlen(msgbuf)-1] = 0;
	msgnw(maint->curwin, msgbuf);
}
#else
#define LOGCSIU(esc, label)
#define LOGESCSEQ(esc)
#endif

static void esc_put(struct esc_parse *esc, int c)
{
	if (esc->end <  KEYSEQ_MAX) esc->buf[esc->end++] = c;
	if (esc->end >= KEYSEQ_MAX) esc->state = ESC_FAIL;
}

static void esc_put_first(struct esc_parse *esc, int c)
{
	esc->end = 0;
	esc_put(esc, c);
}

static void esc_maybe_alt(struct esc_parse *esc, int c)
{
	esc->end = 0;
	if (esc->alt) esc_put(esc, 27);
	if (c >= 0) esc_put(esc, c);
}

static void esc_putnum(struct esc_parse *esc, int n)
{
	int d = 1000000000;
	if (n < 0) n = 1; /* default value */
	while (n / d == 0) d /= 10;
	while (d) { esc_put(esc, '0' + ((n / d) % 10)); d /= 10; }
}

static void esc_putoptnum(struct esc_parse *esc, int n, int sep)
{
	if (n != -1 && n != 1) {
		if (sep != -1) esc_put(esc, sep);
		esc_putnum(esc, n);
	}
}

#if CSI_U_LEVEL >= 4
/* first overrides arg[0] (u.key) if not negative */
static void esc_putnums(struct esc_parse *esc, int first)
{
	int i;
	esc_putnum(esc, first >= 0 ? first : esc->u.key);
	esc_putoptnum(esc, esc->u.shifted, ':');
	if (IS_DEFAULT(esc->u.shifted) && !IS_DEFAULT(esc->u.base))
		esc_put(esc, ':'); /* double colon */
	esc_putoptnum(esc, esc->u.base, ':');
	/* lose the lock bits only */
	esc_putoptnum(esc, MOD_ARG(esc), ';');
	esc_putoptnum(esc, esc->u.event, ':');
	for (i = 0; i < ARG_CP_MAX && !IS_DEFAULT(esc->u.cp[i]); ++i)
		esc_putoptnum(esc, esc->u.cp[i], ':');
}
#endif

/* Looks for CSI-u, parses it if found.
 * Expects the leading ESC to have been read.
 * Will return ESC, the first char of the replacement, or 1<<31 if to be dropped
 */
int esc_fetch(struct esc_parse *esc)
{
	int c;

	esc->state = ESC_START;
	esc->alt = 0;
	esc->argno = 0;
	esc->pos = esc->end = 0;
	for (c = 15; c >= 0; --c)
		esc->arg[c] = -1;
	/* c = -1; */

	while (esc->state < ESC_END) {
		c = ttgetch_now();
		if (c < 0)
			esc->state = ESC_FAIL;
		else
			esc_put(esc, c);

		switch (esc->state) {
		case ESC_START:
			if (c == 27) { esc->alt = 1; esc->state = ESC_STARTED; break; }
			FALLTHROUGH;
		case ESC_STARTED:
			if (c == 'O') esc->state = ESC_LETTER; /* legacy Fn & cursor */
			else if (c == '[') esc->state = ESC_ARGS;
			else esc->state = ESC_FAIL;
			break;
		case ESC_ARGS:
			if (c >= '0' && c <= '9') {
				if (esc->arg[esc->argno] < 0)
					esc->arg[esc->argno] = c - '0';
				else
					esc->arg[esc->argno] = esc->arg[esc->argno] * 10 + c - '0';
			} else if (c == ':' || c == ';') {
				if (nextarg[esc->argno][c & 1])
					esc->argno = nextarg[esc->argno][c & 1];
				else
					esc->state = ESC_FAIL;
			} else if (((c & ~32) >= 'A' && (c & ~32) <= 'Z') || c == '~')
				esc->state = ESC_END;
			else
				esc->state = ESC_FAIL;
			break;
		case ESC_LETTER:
			if ((c & ~32) >= 'A' && (c & 32) <= 'Z') esc->state = ESC_O_SEQ;
			else if (c >= '0' && c <= '9') {
				/* ugh, Konsole doing (e.g.) ESC O param P */
				if (esc->u.mod < 0)
					esc->u.mod = c - '0';
				else
					esc->u.mod = esc->u.mod * 10 + c - '0';
			} else
				esc->state = ESC_FAIL;
			break;
		case ESC_END:
		case ESC_O_SEQ:
		case ESC_FAIL: /* nothing */;
		}
	}

	if (esc->state == ESC_FAIL)
		return 27;

	/* Okay, let's parse it. It may still be replayed so don't flush yet */

	/* Ignore key release events. We really don't want to be receiving these. */
#if CSI_U_LEVEL >= 2
	if (esc->u.event == 3) {
		esc->end = 0;
		return esc->alt ? 27 : 1<<31;
	}
#endif

	/* Check for sequences other than CSI-u */
	/* We'll need to rebuild them.
	 * Key & mod info (trimmed) only should be present. */
	if (c != 'u') {
		LOGESCSEQ(esc);
#if CSI_U_LEVEL >= 1
		if (esc->state == ESC_O_SEQ) {
			/* could be a Konsole special, but the right place for that is key mappings */
			esc_put_first(esc, 'O');
			esc_putoptnum(esc, MOD_ARG(esc), -1);
		} else if (esc->end == 2 && c >= 'P' && c <= 'S') {
			/* ugh, ESC [ P/Q/R/S needs to be translated */
			esc_put_first(esc, 'O');
		} else {
			esc_put_first(esc, '['); /* hmm, Alt -> Esc prefix? */
			if (MOD_ARG(esc) != 1 || (esc->u.key != -1 && esc->u.key != 1))
				esc_putnum(esc, esc->u.key);
			/* lose the lock bits only */
			esc_putoptnum(esc, MOD_ARG(esc), ';');
		}
		esc_put(esc, c);
#endif
		return 27;
	}

	/* Compose strings */
	if (esc->u.key == 0 && esc->u.cp[0] >= 0) {
#if CSI_U_LEVEL >= 4
		int i;
		for (i = 0; i < ARG_CP_MAX && esc->u.cp[i] >= 0; ++i)
			esc->buf[i] = esc->u.cp[i];
		esc->end = i;
		return esc->buf[esc->pos++];
#else
		return 1<<31;
#endif
	}

	/* Anything outside private use areas */
	/* If Ctrl, translate to the corresponding control code
	 * (we don't have the base key with CSI-u mode 0) */
	/* If Alt, prefix with Esc */
	if (esc->u.key < 0xE000 || (esc->u.key >= 0xF900 && esc->u.key < 0xF0000)) {
		/* this starts with '/' */
		static const char ctrl[] = { 31, '0', '1', 0, 27, 28, 29, 30, 31, 127, '9', ':', ';', '<', '=', '>', 127 };
		/* handling max reporting here! */
		int b = (MOD_MASK(esc, MOD_SHIFT) && esc->u.shifted != -1) ? esc->u.shifted : esc->u.key;
		LOGCSIU(esc, "utf-8");
		esc_maybe_alt(esc, -1);
		if (MOD_MASK(esc, MOD_ALT)) /* well, yes, if you type Esc Alt-Ctrl-Shift-A... */
			esc_put(esc, 27);
		if (MOD_MASK(esc, MOD_CTRL)) {
			if (b >= '/' && b <= '?') b = ctrl[b - '/'];
			else if (b >= '@' && b <= '~') b &= 31;
			else if (b == ' ') b = 0;
			/* else untouched */
		}
		esc_put(esc, b);
		return esc->buf[esc->pos++];
	}

	/* Check for keypad ASCII */
	if (esc->u.key >= KEYPAD_KEYS_BASE && esc->u.key <= KEYPAD_KEYS_LAST) {
		/* Translate to normal keypad */
		/* Prefix with Esc if Alt */
		LOGCSIU(esc, "kpkey");
#if CSI_U_LEVEL >= 1
		esc_maybe_alt(esc, -1);
		if (MOD_MASK(esc, MOD_ALT))
			esc_put(esc, 27);
		esc_put(esc, keypad_keys[esc->u.key - KEYPAD_KEYS_BASE]);
		return esc->buf[esc->pos++];
#else
		return 27;
#endif
	}
	/* TODO? Print, Pause */

	/* Handle the Menu key */
	if (esc->u.key == PRINT_PAUSE_MENU_BASE + 2) {
#if CSI_U_LEVEL >= 1
# if CSI_U_LEVEL >= 4
		/* xterm reports Menu as F16 */
		esc->u.key = FUNCTION_KEYS_BASE + 15; /* F16 */
# else
		/* BUG: kitty 0.24.1..0.46.1 reports Menu as CSI 57363 u */
		esc_put_first(esc, '[');
		esc_putnum(esc, 29);
		esc_putoptnum(esc, MOD_ARG(esc), ';');
		esc_put(esc, '~');
		return 27;
# endif
#endif
	}

	/* Check for function keys */
	/* We can rebuild these in the presence of appropriate key definitions */
	/* FIXME: F21-F35? The #if may need adjustment downwards */
	if (esc->u.key >= FUNCTION_KEYS_BASE && esc->u.key <= FUNCTION_KEYS_LAST) {
		LOGCSIU(esc, "func ");
#if CSI_U_LEVEL >= 4
		/* FIXME: for now, rewrite F1..F20 as xterm */
		/* might need to adjust according to termcap/terminfo */
		static const char fnum[] = {
			11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24,
			25, 26, 28, 29, 31, 32, 33, 34
		};
		int k = esc->u.key - FUNCTION_KEYS_BASE;
		if (!MOD_MASK(esc, MOD_KEYS/*_ALL*/) && k < 4) {
			/* CSI P/Q/R/S, no args */
			/* Observed: F1/F2/F4 as CSI P/Q/S but F3 as CSI 13 ~ */
			esc_maybe_alt(esc, 'O');
			esc_put(esc, 'P' + k);
			return 27;
		}
		esc_maybe_alt(esc, '[');
		if (k < 20) {
			esc_putnums(esc, fnum[k]);
			esc_put(esc, '~');
			return 27;
		}
		/* keep the CSI-u numbering for the rest */
		esc_putnums(esc, -1);
		esc_put(esc, c);
#endif
		return 27;
	}

	/* Check for keypad arrows etc. */
	/* We can rebuild these in the presence of appropriate key definitions */
	if (esc->u.key >= KEYPAD_FUNCS_BASE && esc->u.key <= KEYPAD_FUNCS_LAST) {
		/* FIXME: for now, rewrite as xterm */
		/* might need to adjust according to termcap/terminfo */
		LOGCSIU(esc, "kpfun");
#if CSI_U_LEVEL >= 1
		static const char kpfuncs[] = { 'D', 'C', 'A', 'B', 5, 6, 'H', 'F', 2, 3, 'E' };
		char b = kpfuncs[esc->u.key - KEYPAD_FUNCS_BASE];
		esc_maybe_alt(esc, '[');
		if (b < 10) {
			esc_put(esc, b + '0');
			esc_putoptnum(esc, MOD_ARG(esc), ';');
			esc_put(esc, '~');
		} else {
			char m = MOD_ARG(esc);
			if (m != 1)
				esc_put(esc, '1');
			esc_putoptnum(esc, m, ';');
			esc_put(esc, b);
		}
#endif
		return 27;
	}

	/* debug: log the CSI-u for developer reference (unless modifier) */
	if (esc->u.key < KEY_MODS_BASE || esc->u.key > KEY_MODS_LAST)
		LOGCSIU(esc, "warn ");

	esc->end = 0; /* drop the alt too */
	return 1<<31;
}


/* Create a KBD */

KBD *mkkbd(KMAP *kmap)
{
	KBD *kbd = (KBD *) joe_malloc(SIZEOF(KBD));

	kbd->topmap = kmap;
	kbd->curmap = kmap;
	kbd->x = 0;
	return kbd;
}

/* Eliminate a KBD */

void rmkbd(KBD *k)
{
	joe_free(k);
}

/* Process next key for KBD */

MACRO *dokey(KBD *kbd, int n)
{
	KMAP *bind;

	/* If we were passed a negative character */
	if (n < 0)
		n += 256;

	/* If we're starting from scratch, clear the keymap sequence buffer */
	if (kbd->curmap == kbd->topmap)
		kbd->x = 0;

	/* Update cmap if src changed */
	if (kbd->curmap->rtree_version != kbd->curmap->src_version) {
		rtree_clr(&kbd->curmap->rtree);
		rtree_init(&kbd->curmap->rtree);
		/* interval_show(kbd->curmap->src); */
		rtree_build(&kbd->curmap->rtree, kbd->curmap->src);
		/* rtree_show(&kbd->curmap->rtree); */
		rtree_opt(&kbd->curmap->rtree);
		/* rtree_show(&kbd->curmap->rtree); */
		kbd->curmap->rtree_version = kbd->curmap->src_version;
	}
	bind = (KMAP *)rtree_lookup(&kbd->curmap->rtree, n);
	if (!bind)
		bind = (KMAP *)kbd->curmap->dflt;
	if (bind && bind->what == 1) {	/* A prefix key was found */
		kbd->seq[kbd->x++] = n;
		kbd->curmap = bind;
		bind = 0;
	} else {	/* A complete sequence was found */
		kbd->x = 0;
		kbd->curmap = kbd->topmap;
	}

	return (MACRO *)bind;
}

/* Return key code for key name or -1 for syntax error */

static int keyval(char *s)
{
	if (s[0] == 'U' && s[1] == '+') {
		return zhtoi(s + 2);
	} else if (s[0] == '^' && s[1] && !s[2])
		switch (s[1])
		{
		case '?':
			return 127;
		case '#':
			return 0x9B;
		default:
			return s[1] & 0x1F;
		}
	else if ((s[0] == 'S' || s[0] == 's')
		 && (s[1] == 'P' || s[1] == 'p') && !s[2])
		return ' ';
	else if((s[0]=='M'||s[0]=='m') && s[1]) {
		if(!zcmp(s,"MDOWN")) return KEY_MDOWN;
		else if(!zcmp(s,"MWDOWN")) return KEY_MWDOWN;
		else if(!zcmp(s,"MWUP")) return KEY_MWUP;
		else if(!zcmp(s,"MUP")) return KEY_MUP;
		else if(!zcmp(s,"MDRAG")) return KEY_MDRAG;
		else if(!zcmp(s,"M2DOWN")) return KEY_M2DOWN;
		else if(!zcmp(s,"M2UP")) return KEY_M2UP;
		else if(!zcmp(s,"M2DRAG")) return KEY_M2DRAG;
		else if(!zcmp(s,"M3DOWN")) return KEY_M3DOWN;
		else if(!zcmp(s,"M3UP")) return KEY_M3UP;
		else if(!zcmp(s,"M3DRAG")) return KEY_M3DRAG;
		else if(!zcmp(s,"MIDDLEDOWN")) return KEY_MIDDLEDOWN;
		else if(!zcmp(s,"MIDDLEUP")) return KEY_MIDDLEUP;
		else return s[0];
	} else {
		int ch = utf8_decode_string(s);
		if (ch < 0)
			ch = -1;
		return ch;
	}
/*
	 if (s[1] || !s[0])
		return -1;
	else
		return ((unsigned char *)s)[0];
*/
}

/* Create an empty keymap */

KMAP *mkkmap(void)
{
	KMAP *kmap = (KMAP *) joe_calloc(SIZEOF(KMAP), 1);
	kmap->what = 1;
	rtree_init(&kmap->rtree);
	return kmap;
}

/* Eliminate a keymap */

void rmkmap(KMAP *kmap)
{
	struct interval_list *l, *n;
	if (!kmap)
		return;
	for (l = kmap->src; l; l = n) {
		n = l->next;
		if (((KMAP *)l->map)->what == 1) {
			rmkmap((KMAP *)l->map);
		}
		joe_free(l);
	}
	if (kmap->dflt && ((KMAP *)kmap->dflt)->what == 1)
		rmkmap((KMAP *)kmap->dflt);
	rtree_clr(&kmap->rtree);
	joe_free(kmap);
}

/* Parse a range */

static char *range(char *seq, int *vv, int *ww)
{
	char c;
	int x, v, w;

	for (x = 0; seq[x] && seq[x] != ' '; ++x) ;	/* Skip to a space */
	c = seq[x];
	seq[x] = 0;		/* Zero terminate the string */
	v = keyval(seq);	/* Get key */
	w = v;
	if (w < 0)
		return NULL;
	seq[x] = c;		/* Restore the space or 0 */
	for (seq += x; *seq == ' '; ++seq) ;	/* Skip over spaces */

	/* Check for 'TO ' */
	if ((seq[0] == 'T' || seq[0] == 't') && (seq[1] == 'O' || seq[1] == 'o') && seq[2] == ' ') {
		for (seq += 2; *seq == ' '; ++seq) ;	/* Skip over spaces */
		for (x = 0; seq[x] && seq[x] != ' '; ++x) ;	/* Skip to space */
		c = seq[x];
		seq[x] = 0;	/* Zero terminate the string */
		w = keyval(seq);	/* Get key */
		if (w < 0)
			return NULL;
		seq[x] = c;	/* Restore the space or 0 */
		for (seq += x; *seq == ' '; ++seq) ;	/* Skip over spaces */
	}

	if (v > w)
		return NULL;

	*vv = v;
	*ww = w;
	return seq;
}

/* Add a binding to a keymap */

static KMAP *kbuild(CAP *cap, KMAP *kmap, char *seq, MACRO *bind, int *err, const char *capseq, ptrdiff_t seql)
{
	int v, w;

	if (!seql && seq[0] == '.' && seq[1]) {
		int x;
		char c;
		const char *s;
		char *iv;

		for (x = 0; seq[x] && seq[x] != ' '; ++x) ;
		c = seq[x];
		seq[x] = 0;
#ifdef __MSDOS__
		if (!zcmp(seq + 1, "ku")) {
			capseq = "\0H";
			seql = 2;
		} else if (!zcmp(seq + 1, "kd")) {
			capseq = "\0P";
			seql = 2;
		} else if (!zcmp(seq + 1, "kl")) {
			capseq = "\0K";
			seql = 2;
		} else if (!zcmp(seq + 1, "kr")) {
			capseq = "\0M";
			seql = 2;
		} else if (!zcmp(seq + 1, "kI")) {
			capseq = "\0R";
			seql = 2;
		} else if (!zcmp(seq + 1, "kD")) {
			capseq = "\0S";
			seql = 2;
		} else if (!zcmp(seq + 1, "kh")) {
			capseq = "\0G";
			seql = 2;
		} else if (!zcmp(seq + 1, "kH")) {
			capseq = "\0O";
			seql = 2;
		} else if (!zcmp(seq + 1, "kP")) {
			capseq = "\0I";
			seql = 2;
		} else if (!zcmp(seq + 1, "kN")) {
			capseq = "\0Q";
			seql = 2;
		} else if (!zcmp(seq + 1, "k1")) {
			capseq = "\0;";
			seql = 2;
		} else if (!zcmp(seq + 1, "k2")) {
			capseq = "\0<";
			seql = 2;
		} else if (!zcmp(seq + 1, "k3")) {
			capseq = "\0=";
			seql = 2;
		} else if (!zcmp(seq + 1, "k4")) {
			capseq = "\0>";
			seql = 2;
		} else if (!zcmp(seq + 1, "k5")) {
			capseq = "\0?";
			seql = 2;
		} else if (!zcmp(seq + 1, "k6")) {
			capseq = "\0@";
			seql = 2;
		} else if (!zcmp(seq + 1, "k7")) {
			capseq = "\0A";
			seql = 2;
		} else if (!zcmp(seq + 1, "k8")) {
			capseq = "\0B";
			seql = 2;
		} else if (!zcmp(seq + 1, "k9")) {
			capseq = "\0C";
			seql = 2;
		} else if (!zcmp(seq + 1, "k0")) {
			capseq = "\0D";
			seql = 2;
		}
		seq[x] = c;
		if (seql) {
			for (seq += x; *seq == ' '; ++seq) ;
		}
#else
		s = jgetstr(cap, seq + 1);
		seq[x] = c;
		if (s && (iv = tcompile(cap, s, 0, 0, 0, 0))
		    && (sLEN(iv) > 1 || (signed char)iv[0] < 0)) {
			capseq = iv;
			seql = sLEN(iv);
			for (seq += x; *seq == ' '; ++seq) ;
		}
#endif
		else {
			*err = -2;
			return kmap;
		}
	}

	if (seql) {
		v = w = *capseq++;
		--seql;
	} else {
		seq = range(seq, &v, &w);
		if (!seq) {
			*err = -1;
			return kmap;
		}
	}

	if (!kmap)
		kmap = mkkmap();	/* Create new keymap if 'kmap' was NULL */

	/* Make bindings between v and w */
	if (v <= w) {
		if (*seq || seql) {
			KMAP *old = (KMAP *)interval_lookup(kmap->src, NULL, v);
			if (!old || !old->what) {
				kmap->src = interval_add(kmap->src, v, w, kbuild(cap, NULL, seq, bind, err, capseq, seql));
				++kmap->src_version;
			} else
				kbuild(cap, old, seq, bind, err, capseq, seql);
		} else {
			kmap->src = interval_add(kmap->src, v, w, bind);
			++kmap->src_version;
		}
	}
	return kmap;
}

int kadd(CAP *cap, KMAP *kmap, char *seq, MACRO *bind)
{
	int err = 0;

	kbuild(cap, kmap, seq, bind, &err, NULL, 0);
	return err;
}

void kcpy(KMAP *dest, KMAP *src)
{
	struct interval_list *l;
	for (l = src->src; l; l = l->next) {
		if (((KMAP *)l->map)->what == 1) {
			KMAP *k = mkkmap();
			kcpy(k, (KMAP *)l->map);
			dest->src = interval_add(dest->src, l->interval.first, l->interval.last, k);
			++dest->src_version;
		} else {
			dest->src = interval_add(dest->src, l->interval.first, l->interval.last, l->map);
			++dest->src_version;
		}
	}
}

/* Remove a binding from a keymap */

int kdel(KMAP *kmap, char *seq)
{
	int err = 1;
	int v, w;

	seq = range(seq, &v, &w);
	if (!seq)
		return -1;

	/* Clear bindings between v and w */
	if (v <= w) {
		if (*seq) {
			KMAP *old = (KMAP *)interval_lookup(kmap->src, NULL, v);
			if (old->what == 1) {
				kdel(old, seq);
			} else {
				kmap->src = interval_add(kmap->src, v, w, NULL);
				++kmap->src_version;

			}
		} else {
			kmap->src = interval_add(kmap->src, v, w, NULL);
			++kmap->src_version;
		}
	}

	return err;
}

/* Find a context of a given name- if not found, one with an empty kmap
 * is created.
 */

KMAP *kmap_getcontext(const char *name)
{
	struct context *c;

	for (c = contexts; c; c = c->next)
		if (!zcmp(c->name, name))
			return c->kmap;
	c = (struct context *) joe_malloc(SIZEOF(struct context));

	c->next = contexts;
	c->name = zdup(name);
	contexts = c;
	return c->kmap = mkkmap();
}

/* JM - ngetcontext(name) - like getcontext, but return NULL if it
 * doesn't exist, instead of creating a new one.
 */

KMAP *ngetcontext(const char *name)
{
	struct context *c;
	for(c=contexts;c;c=c->next)
		if(!zcmp(c->name,name))
			return c->kmap;
	return 0;
}

/* True if KMAP is empty */

int kmap_empty(KMAP *k)
{
	return k->src == NULL && k->dflt == NULL;
}

/* JM */

B *keymaphist=0;

static int dokeymap(W *w,char *s,void *object,int *notify)
{
	KMAP *k=ngetcontext(s);
	vsrm(s);
	if(notify) *notify=1;
	if(!k) {
		msgnw(w,joe_gettext(_("No such keymap")));
		return -1;
	}
	rmkbd(w->kbd);
	w->kbd=mkkbd(k);
	return 0;
}

static char **get_keymap_list(void)
{
	char **lst = 0;
	struct context *c;
	for (c=contexts; c; c=c->next)
		lst = vaadd(lst, vsncpy(NULL,0,sz(c->name)));

	return lst;
}

static int keymap_cmplt(BW *bw, int k)
{
	/* Reload every time: we should really check date of tags file...
	  if (tag_word_list)
	  	varm(tag_word_list); */

	if (!keymap_list)
		keymap_list = get_keymap_list();

	if (!keymap_list) {
		ttputc(7);
		return 0;
	}

	return simple_cmplt(bw,keymap_list);
}

int ukeymap(W *w, int k)
{
	if (wmkpw(w,joe_gettext(_("Change keymap: ")),&keymaphist,dokeymap,"keymap",NULL,keymap_cmplt,NULL,NULL,locale_map,0)) return 0;
	else return -1;
}
