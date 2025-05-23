/*
 * ahci serial ata driver
 * copyright © 2007-8 coraid, inc.
 */

#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/sd.h"
#include "ahci.h"

#define	dprint(...)	if(debug)	iprint(__VA_ARGS__); else USED(debug)
#define	idprint(...)	if(prid)	iprint(__VA_ARGS__);  else USED(prid)
#define	aprint(...)	if(datapi)	iprint(__VA_ARGS__);  else USED(datapi)

#define Tname(c)	tname[(c)->type]
#define Intel(x)	((x)->pci->vid == 0x8086)

enum {
	NCtlr	= 16,
	NCtlrdrv	= 32,
	NDrive	= NCtlr*NCtlrdrv,

	Fahdrs	= 4,

	Read	= 0,
	Write,

	Eesb	= 1<<0,	/* must have (Eesb & Emtype) == 0 */

	Nms	= 256,			/* ms. between drive checks */
	Mphywait=  2*1024/Nms - 1,
	Midwait	= 16*1024/Nms - 1,
	Mcomrwait= 1*1024/Nms - 1,

	Obs	= 0xa0,			/* obsolete device bits */

	/*
	 * if we get more than this many interrupts per tick for a drive,
	 * either the hardware is broken or we've got a bug in this driver.
	 */
	Maxintrspertick = 2000,		/* was 1000 */
};

/* pci space configuration */
enum {
	Pmap	= 0x90,
	Ppcs	= 0x91,
	Prev	= 0xa8,
};

enum {
	Tesb,
	Tich,
	Tsb600,
	Tjmicron,
	Tahci,
};

static char *tname[] = {
	"63xxesb",
	"ich",
	"sb600",
	"jmicron",
	"ahci",
};

enum {
	Dnull,
	Dmissing,
	Dnew,
	Dready,
	Derror,
	Dreset,
	Doffline,
	Dportreset,
	Dlast,
};

static char *diskstates[Dlast] = {
	"null",
	"missing",
	"new",
	"ready",
	"error",
	"reset",
	"offline",
	"portreset",
};

enum {
	DMautoneg,
	DMsatai,
	DMsataii,
	DMsata3,
};

static char *modename[] = {		/* used in control messages */
	"auto",
	"satai",
	"sataii",
	"sata3",
};
static char *descmode[] = {		/*  only printed */
	"auto",
	"sata 1",
	"sata 2",
	"sata 3",
};

static char *flagname[Dnflag] = {
	"llba",
	"smart",
	"power",
	"nop",
	"atapi",
	"atapi16",
	"sct",
};

typedef struct Htab Htab;
struct Htab {
	ulong	bit;
	char	*name;
};

typedef struct Asleep Asleep;
typedef struct Ctlr Ctlr;
typedef struct Drive Drive;

struct Drive {
	Lock;

	Ctlr	*ctlr;
	SDunit	*unit;
	char	name[10];
	Aport	*port;
	Aportm	portm;
	Aportc	portc;		/* redundant ptr to port and portm */

	uchar	mediachange;
	uchar	state;
	uchar	smartrs;

	uvlong	sectors;
	ulong	secsize;
	ulong	intick;		/* start tick of current transfer */
	ulong	lastseen;
	int	wait;
	uchar	mode;		/* DMautoneg, satai or sataii */
	uchar	active;

	char	serial[20+1];
	char	firmware[8+1];
	char	model[40+1];

	int	infosz;
	ushort	*info;
	ushort	tinyinfo[2];	/* used iff malloc fails */

	int	driveno;	/* ctlr*NCtlrdrv + unit */
	/* controller port # != driveno when not all ports are enabled */
	int	portno;

	ulong	lastintr0;
	ulong	intrs;
};

struct Ctlr {
	Lock;

	int	type;
	int	enabled;
	SDev	*sdev;
	Pcidev	*pci;
	void*	vector;

	/* virtual register addresses */
	uchar	*mmio;
	ulong	*lmmio;
	Ahba	*hba;
	Aenc;
	uint	enctype;

	/* phyical register address */
	uchar	*physio;

	Drive	*rawdrive;
	Drive	*drive[NCtlrdrv];
	int	ndrive;
	int	mport;		/* highest drive # (0-origin) on ich9 at least */

	ulong	lastintr0;
	ulong	intrs;		/* not attributable to any drive */
};

struct Asleep {
	Aport	*p;
	int	i;
};

extern SDifc sdiahciifc;

static	Ctlr	iactlr[NCtlr];
static	SDev	sdevs[NCtlr];
static	int	niactlr;

static	Drive	*iadrive[NDrive];
static	int	niadrive;

/* these are fiddled in iawtopctl() */
static	int	debug;
static	int	prid = 1;
static	int	datapi;

static char stab[] = {
[0]	'i', 'm',
[8]	't', 'c', 'p', 'e',
[16]	'N', 'I', 'W', 'B', 'D', 'C', 'H', 'S', 'T', 'F', 'X'
};

static void
serrstr(ulong r, char *s, char *e)
{
	int i;

	e -= 3;
	for(i = 0; i < nelem(stab) && s < e; i++)
		if(r & (1<<i) && stab[i]){
			*s++ = stab[i];
			if(SerrBad & (1<<i))
				*s++ = '*';
		}
	*s = 0;
}

static char ntab[] = "0123456789abcdef";

static void
preg(uchar *reg, int n)
{
	int i;
	char buf[25*3+1], *e;

	e = buf;
	for(i = 0; i < n; i++){
		*e++ = ntab[reg[i]>>4];
		*e++ = ntab[reg[i]&0xf];
		*e++ = ' ';
	}
	*e++ = '\n';
	*e = 0;
	dprint(buf);
}

static void
dreg(char *s, Aport *p)
{
	dprint("ahci: %stask=%#lux; cmd=%#lux; ci=%#lux; is=%#lux\n",
		s, p->task, p->cmd, p->ci, p->isr);
}

static void
esleep(int ms)
{
	if(waserror())
		return;
	tsleep(&up->sleep, return0, 0, ms);
	poperror();
}

static int
ahciclear(void *v)
{
	Asleep *s;

	s = v;
	return (s->p->ci & s->i) == 0;
}

static void
aesleep(Aportm *pm, Asleep *a, int ms)
{
	if(waserror())
		return;
	tsleep(pm, ahciclear, a, ms);
	poperror();
}

static int
ahciwait(Aportc *c, int ms)
{
	Asleep as;
	Aport *p;

	p = c->p;
	p->ci = 1;
	as.p = p;
	as.i = 1;
	aesleep(c->pm, &as, ms);
	if((p->task&1) == 0 && p->ci == 0)
		return 0;
	dreg("ahciwait timeout ", c->p);
	return -1;
}

/* fill in cfis boilerplate */
static uchar *
cfissetup(Aportc *pc)
{
	uchar *cfis;

	cfis = pc->pm->ctab->cfis;
	memset(cfis, 0, 0x20);
	cfis[0] = 0x27;
	cfis[1] = 0x80;
	cfis[7] = Obs;
	return cfis;
}

/* initialise pc's list */
static void
listsetup(Aportc *pc, int flags)
{
	Alist *list;

	list = pc->pm->list;
	list->flags = flags | 0x5;
	list->len = 0;
	list->ctab = PCIWADDRL(pc->pm->ctab);
	list->ctabhi = PCIWADDRH(pc->pm->ctab);
}

static int
nop(Aportc *pc)
{
	uchar *c;

	if((pc->pm->feat & Dnop) == 0)
		return -1;
	c = cfissetup(pc);
	c[2] = 0;
	listsetup(pc, Lwrite);
	return ahciwait(pc, 3*1000);
}

static int
setfeatures(Aportc *pc, uchar f)
{
	uchar *c;

	c = cfissetup(pc);
	c[2] = 0xef;
	c[3] = f;
	listsetup(pc, Lwrite);
	return ahciwait(pc, 3*1000);
}

static int
setudmamode(Aportc *pc, uchar f)
{
	uchar *c;
	int m;

	/* hack */
	if((pc->p->sig >> 16) == 0xeb14)
		return 0;
	m = 0x40;
	if(f == 0xff){
		f = 0;
		m = 0;
	}
	c = cfissetup(pc);
	c[2] = 0xef;
	c[3] = 3;		/* set transfer mode */
	c[12] = m | f;	/* sector count */
	listsetup(pc, Lwrite);
	return ahciwait(pc, 3*1000);
}

static void
asleep(int ms)
{
	if(up == nil || !islo())
		delay(ms);
	else
		esleep(ms);
}

static void
ahciportreset(Aportc *c, uint mode)
{
	ulong *cmd, i;
	Aport *p;

	p = c->p;
	cmd = &p->cmd;
	*cmd &= ~(Afre|Ast);
	for(i = 0; i < 500; i += 25){
		if((*cmd&Acr) == 0)
			break;
		asleep(25);
	}
	if((*cmd & Apwr) != Apwr)
		*cmd |= Apwr;
	p->sctl = 3*Aipm | 0*Aspd | Adet;
	delay(1);
	p->sctl = 3*Aipm | mode*Aspd;
}

static int
smart(Aportc *pc, int n)
{
	uchar *c;

	if((pc->pm->feat&Dsmart) == 0)
		return -1;
	c = cfissetup(pc);
	c[2] = 0xb0;
	c[3] = 0xd8 + n;	/* able smart */
	c[5] = 0x4f;
	c[6] = 0xc2;
	listsetup(pc, Lwrite);
	if(ahciwait(pc, 1000) == -1 || pc->p->task & (1|32)){
		dprint("ahci: smart fail %#lux\n", pc->p->task);
//		preg(pc->pm->fis.r, 20);
		return -1;
	}
	if(n)
		return 0;
	return 1;
}

static int
smartrs(Aportc *pc)
{
	uchar *c;

	c = cfissetup(pc);
	c[2] = 0xb0;
	c[3] = 0xda;		/* return smart status */
	c[5] = 0x4f;
	c[6] = 0xc2;
	listsetup(pc, Lwrite);

	c = pc->pm->fis.r;
	if(ahciwait(pc, 1000) == -1 || pc->p->task & (1|32)){
		dprint("ahci: smart fail %#lux\n", pc->p->task);
		preg(c, 20);
		return -1;
	}
	if(c[5] == 0x4f && c[6] == 0xc2)
		return 1;
	return 0;
}

static int
ahciflushcache(Aportc *pc)
{
	uchar *c;

	c = cfissetup(pc);
	c[2] = pc->pm->feat & Dllba? 0xea: 0xe7;
	listsetup(pc, Lwrite);
	if(ahciwait(pc, 60000) == -1 || pc->p->task & (1|32)){
		dprint("ahciflushcache: fail %#lux\n", pc->p->task);
//		preg(pc->pm->fis.r, 20);
		return -1;
	}
	return 0;
}

static ushort
gbit16(void *a)
{
	uchar *i;

	i = a;
	return i[1]<<8 | i[0];
}

static ulong
gbit32(void *a)
{
	ulong j;
	uchar *i;

	i = a;
	j  = i[3] << 24;
	j |= i[2] << 16;
	j |= i[1] << 8;
	j |= i[0];
	return j;
}

static uvlong
gbit64(void *a)
{
	uchar *i;

	i = a;
	return (uvlong)gbit32(i+4) << 32 | gbit32(a);
}

static int
ahciidentify0(Aportc *pc, void *id, int atapi)
{
	uchar *c;
	Aprdt *p;
	static uchar tab[] = { 0xec, 0xa1, };

	c = cfissetup(pc);
	c[2] = tab[atapi];
	listsetup(pc, 1<<16);

	memset(id, 0, 0x100);			/* magic */
	p = &pc->pm->ctab->prdt;
	p->dba = PCIWADDRL(id);
	p->dbahi = PCIWADDRH(id);
	p->count = 1<<31 | (0x200-2) | 1;
	return ahciwait(pc, 3*1000);
}

static vlong
ahciidentify(Aportc *pc, ushort *id)
{
	int i, sig;
	vlong s;
	Aportm *pm;

	pm = pc->pm;
	pm->feat = 0;
	pm->smart = 0;
	i = 0;
	sig = pc->p->sig >> 16;
	if(sig == 0xeb14){
		pm->feat |= Datapi;
		i = 1;
	}
	if(ahciidentify0(pc, id, i) == -1)
		return -1;

	i = gbit16(id+83) | gbit16(id+86);
	if(i & Illba){
		pm->feat |= Dllba;
		s = gbit64(id+100);
	}else
		s = gbit32(id+60);

	i = gbit16(id + 49);
	pm->udma = 0xff;
	if(i & Idmasp)
		if(gbit16(id + 53) & 4)
			for(i = gbit16(id + 88) & 0x7f; i; i >>= 1)
				pm->udma++;

	if(pm->feat&Datapi){
		i = gbit16(id+0);
		if(i&1)
			pm->feat |= Datapi16;
	}

	i = gbit16(id+83);
	if((i>>14) == 1) {
		if(i & (1<<3))
			pm->feat |= Dpower;
		i = gbit16(id+82);
		if(i & 1)
			pm->feat |= Dsmart;
		if(i & (1<<14))
			pm->feat |= Dnop;
	}

	if(gbit16(id + 206) & 1)
		pm->feat |= Dsct;
	return s;
}

static int
ahciquiet(Aport *a)
{
	ulong *p, i;

	p = &a->cmd;
	*p &= ~Ast;
	for(i = 0; i < 500; i += 50){
		if((*p & Acr) == 0)
			goto stop;
		asleep(50);
	}
	return -1;
stop:
	if((a->task & (ASdrq|ASbsy)) == 0){
		*p |= Ast;
		return 0;
	}

	*p |= Aclo;
	for(i = 0; i < 500; i += 50){
		if((*p & Aclo) == 0)
			goto stop1;
		asleep(50);
	}
	return -1;
stop1:
	/* extra check */
	dprint("ahci: clo clear %#lx\n", a->task);
	if(a->task & ASbsy)
		return -1;
	*p |= Afre|Ast;
	return 0;
}

static int
ahcicomreset(Aportc *pc)
{
	uchar *c;

	dprint("ahcicomreset\n");
	dreg("ahci: comreset ", pc->p);
	if(ahciquiet(pc->p) == -1){
		dprint("ahciquiet failed\n");
		return -1;
	}
	dreg("comreset ", pc->p);

	c = cfissetup(pc);
	c[1] = 0;
	c[15] = 1<<2;		/* srst */
	listsetup(pc, Lclear | Lreset);
	if(ahciwait(pc, 500) == -1){
		dprint("ahcicomreset: first command failed\n");
		return -1;
	}
	microdelay(250);
	dreg("comreset ", pc->p);

	c = cfissetup(pc);
	c[1] = 0;
	listsetup(pc, Lwrite);
	if(ahciwait(pc, 150) == -1){
		dprint("ahcicomreset: second command failed\n");
		return -1;
	}
	dreg("comreset ", pc->p);
	return 0;
}

static int
ahciidle(Aport *port)
{
	ulong *p, i, r;

	p = &port->cmd;
	if((*p & Arun) == 0)
		return 0;
	*p &= ~Ast;
	r = 0;
	for(i = 0; i < 500; i += 25){
		if((*p & Acr) == 0)
			goto stop;
		asleep(25);
	}
	r = -1;
stop:
	if((*p & Afre) == 0)
		return r;
	*p &= ~Afre;
	for(i = 0; i < 500; i += 25){
		if((*p & Afre) == 0)
			return 0;
		asleep(25);
	}
	return -1;
}

/*
 * § 6.2.2.1  first part; comreset handled by reset disk.
 *	- remainder is handled by configdisk.
 *	- ahcirecover is a quick recovery from a failed command.
 */
static int
ahciswreset(Aportc *pc)
{
	int i;

	i = ahciidle(pc->p);
	pc->p->cmd |= Afre;
	if(i == -1)
		return -1;
	if(pc->p->task & (ASdrq|ASbsy))
		return -1;
	return 0;
}

static int
ahcirecover(Aportc *pc)
{
	ahciswreset(pc);
	pc->p->cmd |= Ast;
	if(setudmamode(pc, pc->pm->udma) == -1)
		return -1;
	return 0;
}

static void*
malign(int size, int align)
{
	return mallocalign(size, align, 0, 0);
}

static void
setupfis(Afis *f)
{
	f->base = malign(0x100, 0x100);		/* magic */
	f->d = f->base + 0;
	f->p = f->base + 0x20;
	f->r = f->base + 0x40;
	f->u = f->base + 0x60;
	f->devicebits = (ulong*)(f->base + 0x58);
}

static void
ahciwakeup(Aportc *c, uint mode)
{
	ushort s;
	Aport *p;

	p = c->p;
	s = p->sstatus;
	if((s & Intpm) != Intslumber && (s & Intpm) != Intpartpwr)
		return;
	if((s & Devdet) != Devpresent){	/* not (device, no phy) */
		iprint("ahci: slumbering drive unwakable %#ux\n", s);
		return;
	}
	ahciportreset(c, mode);
	iprint("ahci: wake %#ux -> %#lux\n", s, p->sstatus);
}

static int
ahciconfigdrive(Drive *d)
{
	Ahba *h;
	Aport *p;
	Aportm *pm;
	int i;

	h = d->ctlr->hba;
	p = d->portc.p;
	pm = d->portc.pm;
	if(pm->list == 0){
		setupfis(&pm->fis);
		pm->list = malign(sizeof *pm->list, 1024);
		pm->ctab = malign(sizeof *pm->ctab, 128);
	}

	if(ahciidle(p) == -1){
		dprint("ahci: port not idle\n");
		return -1;
	}

	p->list = PCIWADDRL(pm->list);
	p->listhi = PCIWADDRH(pm->list);
	p->fis = PCIWADDRL(pm->fis.base);
	p->fishi = PCIWADDRH(pm->fis.base);

	p->cmd |= Afre;

	if((p->cmd & Apwr) != Apwr)
		p->cmd |= Apwr;

	if((h->cap & Hsss) != 0){
		dprint("ahci: spin up ... [%.3lux]\n", p->sstatus);
		for(i = 0; i < 1400; i += 50){
			if((p->sstatus & Devphyoffline) != 0)
				break;
			if((p->sstatus & Devdet) == (Devphycomm|Devpresent))
				break;
			asleep(50);
		}
	}

	/* drive coming up in slumbering? */
	if((p->sstatus & Devdet) == Devpresent &&
	   ((p->sstatus & Intpm) == Intslumber ||
	    (p->sstatus & Intpm) == Intpartpwr))
		ahciwakeup(&d->portc, d->mode);

	p->serror = SerrAll;
	p->ie = IEM;

	/* we will get called again once phylink has been established */
	if((p->sstatus & Devdet) != (Devphycomm|Devpresent))
		return 0;

	/* "disable power managment" sequence from book. */
	p->sctl = (3*Aipm) | (d->mode*Aspd) | (0*Adet);
	p->cmd &= ~Aalpe;

	p->cmd |= Afre|Ast;

	return 0;
}

static void
ahcienable(Ahba *h)
{
	h->ghc |= Hie;
}

static void
ahcidisable(Ahba *h)
{
	h->ghc &= ~Hie;
}

static int
countbits(ulong u)
{
	int n;

	n = 0;
	for (; u != 0; u >>= 1)
		if(u & 1)
			n++;
	return n;
}

static int
ahciconf(Ctlr *ctlr)
{
	Ahba *h;
	ulong u;

	h = ctlr->hba = (Ahba*)ctlr->mmio;
	u = h->cap;

	if((u&Hsam) == 0)
		h->ghc |= Hae;

	dprint("#S/sd%c: type %s port %#p: sss %ld ncs %ld coal %ld "
		"%ld ports, led %ld clo %ld ems %ld\n",
		ctlr->sdev->idno, tname[ctlr->type], h,
		(u>>27) & 1, (u>>8) & 0x1f, (u>>7) & 1,
		(u & 0x1f) + 1, (u>>25) & 1, (u>>24) & 1, (u>>6) & 1);
	return countbits(h->pi);
}

static int
ahcihandoff(Ahba *h)
{
	int wait;

	if((h->cap2 & Boh) == 0)
		return 0;
	h->bios |= Oos;
	for(wait = 0; wait < 2000; wait += 100){
		if((h->bios & Bos) == 0)
			return 0;
		delay(100);
	}
	iprint("ahci: bios handoff timed out\n");
	return -1;
}

static int
ahcihbareset(Ahba *h)
{
	int wait;

	h->ghc |= Hhr;
	for(wait = 0; wait < 1000; wait += 100){
		if((h->ghc & Hhr) == 0)
			return 0;
		delay(100);
	}
	return -1;
}

static char*
dnam(Drive *d)
{
	char *s;

	s = d->name;
	if(d->unit && d->unit->name)
		s = d->unit->name;
	return s;
}

static void
idmove(char *p, ushort *a, int n)
{
	int i;
	char *op, *e;

	op = p;
	for(i = 0; i < n/2; i++){
		*p++ = a[i] >> 8;
		*p++ = a[i];
	}
	*p = 0;
	while(p > op && *--p == ' ')
		*p = 0;
	e = p;
	for (p = op; *p == ' '; p++)
		;
	memmove(op, p, n - (e - p));
}

static int
identify(Drive *d)
{
	ushort *id;
	vlong osectors, s;
	uchar oserial[21];
	SDunit *u;

	if(d->info == nil) {
		d->infosz = 512 * sizeof(ushort);
		d->info = malloc(d->infosz);
	}
	if(d->info == nil) {
		d->info = d->tinyinfo;
		d->infosz = sizeof d->tinyinfo;
	}
	id = d->info;
	s = ahciidentify(&d->portc, id);
	if(s == -1){
		d->state = Derror;
		return -1;
	}
	osectors = d->sectors;
	memmove(oserial, d->serial, sizeof d->serial);

	u = d->unit;
	d->sectors = s;
	d->secsize = u->secsize;
	if(d->secsize == 0)
		d->secsize = 512;		/* default */
	d->smartrs = 0;

	idmove(d->serial, id+10, 20);
	idmove(d->firmware, id+23, 8);
	idmove(d->model, id+27, 40);

	memset(u->inquiry, 0, sizeof u->inquiry);
	u->inquiry[2] = 2;
	u->inquiry[3] = 2;
	u->inquiry[4] = sizeof u->inquiry - 4;
	memmove(u->inquiry+8, d->model, 40);

	if(osectors != s || memcmp(oserial, d->serial, sizeof oserial) != 0){
		d->mediachange = 1;
		u->sectors = 0;
	}
	return 0;
}

static void
clearci(Aport *p)
{
	if(p->cmd & Ast) {
		p->cmd &= ~Ast;
		p->cmd |=  Ast;
	}
}

static int
ignoreahdrs(Drive *d)
{
	return d->portm.feat & Datapi && d->ctlr->type == Tsb600;
}

static void
updatedrive(Drive *d)
{
	ulong f, cause, serr, s0, pr, ewake;
	char *name;
	Aport *p;
	static ulong last;

	pr = 1;
	ewake = 0;
	f = 0;
	p = d->port;
	cause = p->isr;
	if(d->ctlr->type == Tjmicron)
		cause &= ~Aifs;
	serr = p->serror;
	p->isr = cause;
	name = dnam(d);

	if(p->ci == 0){
		f |= Fdone;
		wakeup(&d->portm);
		pr = 0;
	}else if(cause & Adps){
		pr = 0;
	}else if(cause & Atfes){
		f |= Ferror;
		ewake = 1;
		pr = 0;
	}
	if(cause & Ifatal){
		ewake = 1;
		dprint("ahci: updatedrive: %s: fatal\n", name);
	}
	if(cause & Adhrs){
		if(p->task & (1<<5|1)){
			if(ignoreahdrs(d) && serr & ErrE)
				f |= Fahdrs;
			dprint("ahci: %s: Adhrs cause %#lux serr %#lux task %#lux\n",
				name, cause, serr, p->task);
			f |= Ferror;
			ewake = 1;
		}
		pr = 0;
	}
	if(p->task & 1 && last != cause)
		dprint("%s: err ca %#lux serr %#lux task %#lux sstat %#lux\n",
			name, cause, serr, p->task, p->sstatus);
	if(pr)
		dprint("%s: upd %#lux ta %#lux\n", name, cause, p->task);

	if(cause & (Aprcs|Aifs)){
		s0 = d->state;
		switch(p->sstatus & Devdet){
		case 0:				/* no device */
			d->state = Dmissing;
			break;
		case Devpresent:		/* device but no phy comm. */
			if((p->sstatus & Intpm) == Intslumber ||
			   (p->sstatus & Intpm) == Intpartpwr)
				d->state = Dnew;	/* slumbering */
			else
				d->state = Derror;
			break;
		case Devpresent|Devphycomm:
			/* power mgnt crap for surprise removal */
			p->ie |= Aprcs|Apcs;	/* is this required? */
			d->state = Dreset;
			break;
		case Devphyoffline:
			d->state = Doffline;
			break;
		}
		dprint("%s: %s → %s [Apcrs] %#lux\n", name,
			diskstates[s0], diskstates[d->state], p->sstatus);
		/* print pulled message here. */
		if(s0 == Dready && d->state != Dready)
			idprint("%s: pulled\n", name);		/* wtf? */
		if(d->state != Dready)
			f |= Ferror;
		if(d->state != Dready || p->ci)
			ewake = 1;
	}
	p->serror = serr;
	if(ewake)
		clearci(p);
	if(f){
		d->portm.flag = f;
		wakeup(&d->portm);
	}
	last = cause;
}

static void
pstatus(Drive *d, ulong s)
{
	/*
	 * s is masked with Devdet.
	 *
	 * bogus code because the first interrupt is currently dropped.
	 * likely my fault.  serror may be cleared at the wrong time.
	 */
	switch(s){
	case 0:			/* no device */
		d->state = Dmissing;
		break;
	case Devpresent:	/* device but no phy. comm. */
		break;
	case Devphycomm:	/* should this be missing?  need testcase. */
		dprint("ahci: pstatus 2\n");
		/* fallthrough */
	case Devpresent|Devphycomm:
		d->wait = 0;
		d->state = Dnew;
		break;
	case Devphyoffline:
		d->state = Doffline;
		break;
	case Devphyoffline|Devphycomm:	/* does this make sense? */
		d->state = Dnew;
		break;
	}
}

static int
configdrive(Drive *d)
{
	if(ahciconfigdrive(d) == -1)
		return -1;
	ilock(d);
	pstatus(d, d->port->sstatus & Devdet);
	iunlock(d);
	return 0;
}

static void
setstate(Drive *d, int state)
{
	ilock(d);
	d->state = state;
	iunlock(d);
}

static void
resetdisk(Drive *d)
{
	uint state, det, stat;
	Aport *p;

	p = d->port;
	det = p->sctl & 7;
	stat = p->sstatus & Devdet;
	state = (p->cmd>>28) & 0xf;
	dprint("ahci: resetdisk: icc %#ux  det %d sdet %d\n", state, det, stat);

	ilock(d);
	state = d->state;
	if(d->state != Dready || d->state != Dnew)
		d->portm.flag |= Ferror;
	clearci(p);			/* satisfy sleep condition. */
	wakeup(&d->portm);
	if(stat != (Devpresent|Devphycomm)){
		/* device absent or phy not communicating */
		d->state = Dportreset;
		iunlock(d);
		return;
	}
	d->state = Derror;
	iunlock(d);

	qlock(&d->portm);
	if(p->cmd&Ast && ahciswreset(&d->portc) == -1)
		setstate(d, Dportreset);	/* get a bigger stick. */
	else {
		setstate(d, Dmissing);
		configdrive(d);
	}
	dprint("ahci: %s: resetdisk: %s → %s\n", dnam(d),
		diskstates[state], diskstates[d->state]);
	qunlock(&d->portm);
}

static int
newdrive(Drive *d)
{
	char *name;
	Aportc *c;
	Aportm *pm;

	c = &d->portc;
	pm = &d->portm;
	name = dnam(d);

	if(d->port->task == 0x80)
		return -1;
	qlock(c->pm);
	if(setudmamode(c, pm->udma) == -1){
		dprint("%s: can't set udma mode\n", name);
		goto lose;
	}
	if(identify(d) == -1){
		dprint("%s: identify failure\n", name);
		goto lose;
	}
	if(pm->feat & Dpower && setfeatures(c, 0x85) == -1){
		pm->feat &= ~Dpower;
		if(ahcirecover(c) == -1)
			goto lose;
	}
	setstate(d, Dready);
	qunlock(c->pm);

	idprint("%s: %sLBA %,llud sectors: %s %s %s %s\n", name,
		(pm->feat & Dllba? "L": ""), d->sectors, d->model, d->firmware,
		d->serial, d->mediachange? "[mediachange]": "");
	return 0;

lose:
	idprint("%s: can't be initialized\n", name);
	setstate(d, Dnull);
	qunlock(c->pm);
	return -1;
}

static void
westerndigitalhung(Drive *d)
{
	if((d->portm.feat&Datapi) == 0 && d->active &&
	    TK2MS(sys->ticks - d->intick) > 5000){
		dprint("%s: drive hung; resetting [%#lux] ci %#lx\n",
			dnam(d), d->port->task, d->port->ci);
		d->state = Dreset;
	}
}

static ushort olds[NCtlr*NCtlrdrv];

static void
doportreset(Drive *d)
{
	qlock(&d->portm);
	ahciportreset(&d->portc, d->mode);
	qunlock(&d->portm);

	dprint("ahci: doportreset: portreset → %s  [task %lux; ss %.3lux]\n",
		diskstates[d->state], d->port->task, d->port->sstatus);
}

/* drive must be locked */
static void
statechange(Drive *d)
{
	switch(d->state){
	case Dnull:
	case Doffline:
		if(d->unit->sectors != 0){
			d->sectors = 0;
			d->mediachange = 1;
		}
		/* fallthrough */
	case Dready:
		d->wait = 0;
		break;
	}
}

static uint
maxmode(Ctlr *c)
{
	return (c->hba->cap & 0xf*Hiss)/Hiss;
}

static void
checkdrive(Drive *d, int i)
{
	ushort s, sig;
	char *name;

	if(d == nil) {
		print("checkdrive: nil d\n");
		return;
	}
	ilock(d);
	if(d->unit == nil || d->port == nil) {
		if(0)
			print("checkdrive: nil d->%s\n",
				d->unit == nil? "unit": "port");
		iunlock(d);
		return;
	}
	name = dnam(d);
	s = d->port->sstatus;
	if(s)
		d->lastseen = sys->ticks;
	if(s != olds[i]){
		dprint("%s: status: %06#ux -> %06#ux: %s\n",
			name, olds[i], s, diskstates[d->state]);
		olds[i] = s;
		d->wait = 0;
	}
	westerndigitalhung(d);

	switch(d->state){
	case Dnull:
	case Dready:
		break;
	case Dmissing:
	case Dnew:
		switch(s & (Intactive | Devdet)){
		case Devpresent:  /* no device (pm), device but no phy. comm. */
			ahciwakeup(&d->portc, d->mode);
			/* fall through */
		case 0:			/* no device */
			break;
		default:
			dprint("%s: unknown status %06#ux\n", name, s);
			/* fall through */
		case Intactive:		/* active, no device */
			if(++d->wait&Mphywait)
				break;
reset:
			if(d->mode == 0)
				d->mode = maxmode(d->ctlr);
			else
				d->mode--;
			if(d->mode == DMautoneg){
				d->wait = 0;
				d->state = Dportreset;
				goto portreset;
			}
			dprint("%s: reset; new mode %s\n", name,
				modename[d->mode]);
			iunlock(d);
			resetdisk(d);
			ilock(d);
			break;
		case Intactive|Devphycomm|Devpresent:
			if(d->unit == nil)
				break;
			if((++d->wait&Midwait) == 0){
				dprint("%s: slow reset %06#ux task=%#lux; %d\n",
					name, s, d->port->task, d->wait);
				goto reset;
			}
			s = (uchar)d->port->task;
			sig = d->port->sig >> 16;
			if(s == 0x7f || s&ASbsy ||
			    (sig != 0xeb14 && (s & ASdrdy) == 0))
				break;
			iunlock(d);
			newdrive(d);
			ilock(d);
			break;
		}
		break;
	case Doffline:
		if(d->wait++ & Mcomrwait)
			break;
	case Derror:
	case Dreset:
		dprint("%s: reset [%s]: mode %d; status %06#ux\n",
			name, diskstates[d->state], d->mode, s);
		iunlock(d);
		resetdisk(d);
		ilock(d);
		break;
	case Dportreset:
portreset:
		if(d->wait++ & Mcomrwait)
			break;
		if(d->wait > Mcomrwait && (s & Intactive) == 0){
			d->state = Dnull;	/* stuck in portreset */
			break;
		}
		dprint("%s: portreset [%s]: mode %d; status %06#ux\n",
			name, diskstates[d->state], d->mode, s);
		d->portm.flag |= Ferror;
		clearci(d->port);
		wakeup(&d->portm);
		if((s & Devdet) == 0){	/* no device */
			d->state = Dmissing;
			break;
		}
		iunlock(d);
		doportreset(d);
		ilock(d);
		break;
	}
	statechange(d);
	iunlock(d);
}

static void
satakproc(void*)
{
	int i;

	while(waserror())
		;
	for(;;){
		tsleep(&up->sleep, return0, 0, Nms);
		for(i = 0; i < niadrive; i++)
			if(iadrive[i] != nil)
				checkdrive(iadrive[i], i);
	}
}

static void
isctlrjabbering(Ctlr *c, ulong cause)
{
	ulong now;

	now = TK2MS(sys->ticks);
	if (now > c->lastintr0) {
		c->intrs = 0;
		c->lastintr0 = now;
	}
	if (++c->intrs > Maxintrspertick) {
		iprint("sdiahci: %lud intrs per tick for no serviced "
			"drive; cause %#lux mport %d\n",
			c->intrs, cause, c->mport);
		c->intrs = 0;
	}
}

static void
isdrivejabbering(Drive *d)
{
	ulong now;

	now = TK2MS(sys->ticks);
	if (now > d->lastintr0) {
		d->intrs = 0;
		d->lastintr0 = now;
	}
	if (++d->intrs > Maxintrspertick) {
		iprint("sdiahci: %lud interrupts per tick for %s\n",
			d->intrs, dnam(d));
		d->intrs = 0;
	}
}

static void
iainterrupt(Ureg*, void *a)
{
	int i;
	ulong cause, mask;
	Ctlr *c;
	Drive *d;

	c = a;
	ilock(c);
	cause = c->hba->isr;
	if (cause == 0) {
		isctlrjabbering(c, cause);
		// iprint("sdiahci: interrupt for no drive\n");
		iunlock(c);
		return;
	}
	for(i = 0; cause && i <= c->mport; i++){
		mask = 1 << i;
		if((cause & mask) == 0)
			continue;
		d = c->rawdrive + i;
		ilock(d);
		isdrivejabbering(d);
		if(d->port != nil && d->port->isr && c->hba->pi & mask)
			updatedrive(d);
		c->hba->isr = mask;
		iunlock(d);

		cause &= ~mask;
	}
	if (cause) {
		isctlrjabbering(c, cause);
		iprint("sdiachi: intr cause unserviced: %#lux\n", cause);
	}
	iunlock(c);
}

/* checkdrive, called from satakproc, will prod the drive while we wait */
static void
awaitspinup(Drive *d)
{
	int ms;
	ushort s;
	char *name;

	ilock(d);
	if(d->unit == nil || d->port == nil) {
		panic("awaitspinup: nil d->unit or d->port");
		iunlock(d);
		return;
	}
	name = dnam(d);
	s = d->port->sstatus;
	if(!(s & Devpresent)) {			/* never going to be ready */
		dprint("awaitspinup: %s absent, not waiting\n", name);
		iunlock(d);
		return;
	}

	for (ms = 20000; ms > 0; ms -= 50)
		switch(d->state){
		case Dnull:
			/* absent; done */
			iunlock(d);
			dprint("awaitspinup: %s in null state\n", name);
			return;
		case Dready:
		case Dnew:
			if(d->sectors || d->mediachange) {
				/* ready to use; done */
				iunlock(d);
				dprint("awaitspinup: %s ready!\n", name);
				return;
			}
			/* fall through */
		default:
		case Dmissing:			/* normal waiting states */
		case Dreset:
		case Doffline:			/* transitional states */
		case Derror:
		case Dportreset:
			iunlock(d);
			asleep(50);
			ilock(d);
			break;
		}
	print("awaitspinup: %s didn't spin up after 20 seconds\n", name);
	iunlock(d);
}

static int
iaverify(SDunit *u)
{
	Ctlr *c;
	Drive *d;

	c = u->dev->ctlr;
	d = c->drive[u->subno];
	ilock(c);
	ilock(d);
	if(d->unit == nil)
		d->unit = u;
	iunlock(d);
	iunlock(c);
	checkdrive(d, d->driveno);		/* c->d0 + d->driveno */

	/*
	 * hang around until disks are spun up and thus available as
	 * nvram, dos file systems, etc.  you wouldn't expect it, but
	 * the intel 330 ssd takes a while to `spin up'.
	 */
	awaitspinup(d);
	return 1;
}

static int
iaenable(SDev *s)
{
	char name[32];
	Ctlr *c;
	static int once;

	c = s->ctlr;
	ilock(c);
	if(!c->enabled) {
		if(once == 0) {
			once = 1;
			kproc("ahci", satakproc, 0);
		}
		if(c->ndrive == 0)
			panic("iaenable: zero s->ctlr->ndrive");
		snprint(name, sizeof name, "%s (%s)", s->name, s->ifc->name);
		c->vector = intrenable(c->pci->intl, iainterrupt, c, c->pci->tbdf, name);
		/* supposed to squelch leftover interrupts here. */
		ahcienable(c->hba);
		c->enabled = 1;
	}
	iunlock(c);
	return 1;
}

static int
iadisable(SDev *s)
{
	char name[32];
	Ctlr *c;

	c = s->ctlr;
	ilock(c);
	ahcidisable(c->hba);
	snprint(name, sizeof name, "%s (%s)", s->name, s->ifc->name);
	intrdisable(c->vector);
	c->enabled = 0;
	iunlock(c);
	return 1;
}

static int
iaonline(SDunit *unit)
{
	int r;
	Ctlr *c;
	Drive *d;

	c = unit->dev->ctlr;
	d = c->drive[unit->subno];
	r = 0;

	if(d->portm.feat & Datapi && d->mediachange){
		r = scsionline(unit);
		if(r > 0)
			d->mediachange = 0;
		return r;
	}

	ilock(d);
	if(d->mediachange){
		r = 2;
		d->mediachange = 0;
		/* devsd resets this after online is called; why? */
		unit->sectors = d->sectors;
		unit->secsize = d->secsize;
	} else if(d->state == Dready)
		r = 1;
	iunlock(d);
	return r;
}

/* returns locked list! */
static Alist*
ahcibuild(Drive *d, uchar *cmd, void *data, int n, vlong lba)
{
	uchar *c, acmd, dir, llba;
	Alist *l;
	Actab *t;
	Aportm *pm;
	Aprdt *p;
	static uchar tab[2][2] = { 0xc8, 0x25, 0xca, 0x35, };

	pm = &d->portm;
	dir = *cmd == ScmdExtwrite || *cmd == ScmdWrite16;
	llba = pm->feat&Dllba? 1: 0;
	acmd = tab[dir][llba];
	qlock(pm);
	l = pm->list;
	t = pm->ctab;
	c = t->cfis;

	c[0] = 0x27;
	c[1] = 0x80;
	c[2] = acmd;
	c[3] = 0;

	c[4] = lba;		/* sector		lba low	7:0 */
	c[5] = lba >> 8;	/* cylinder low		lba mid	15:8 */
	c[6] = lba >> 16;	/* cylinder hi		lba hi	23:16 */
	c[7] = Obs | 0x40;	/* 0x40 == lba */
	if(llba == 0)
		c[7] |= (lba>>24) & 0xf;

	c[8] = lba >> 24;	/* sector (exp)		lba 	31:24 */
	c[9] = lba >> 32;	/* cylinder low (exp)	lba	39:32 */
	c[10] = lba >> 40;	/* cylinder hi (exp)	lba	47:40 */
	c[11] = 0;		/* features (exp); */

	c[12] = n;		/* sector count */
	c[13] = n >> 8;		/* sector count (exp) */
	c[14] = 0;		/* r */
	c[15] = 0;		/* control */

	*(ulong*)(c + 16) = 0;

	l->flags = 1<<16 | Lpref | 0x5;	/* Lpref ?? */
	if(dir == Write)
		l->flags |= Lwrite;
	l->len = 0;
	l->ctab = PCIWADDRL(t);
	l->ctabhi = PCIWADDRH(t);

	p = &t->prdt;
	p->dba = PCIWADDRL(data);
	p->dbahi = PCIWADDRH(data);
	if(d->unit == nil)
		panic("ahcibuild: nil d->unit");
	p->count = 1<<31 | (d->unit->secsize*n - 2) | 1;

	return l;
}

static Alist*
ahcibuildpkt(Aportm *pm, SDreq *r, void *data, int n)
{
	int fill, len;
	uchar *c;
	Alist *l;
	Actab *t;
	Aprdt *p;

	qlock(pm);
	l = pm->list;
	t = pm->ctab;
	c = t->cfis;

	fill = pm->feat&Datapi16? 16: 12;
	if((len = r->clen) > fill)
		len = fill;
	memmove(t->atapi, r->cmd, len);
	memset(t->atapi+len, 0, fill-len);

	c[0] = 0x27;
	c[1] = 0x80;
	c[2] = 0xa0;
	if(n != 0)
		c[3] = 1;	/* dma */
	else
		c[3] = 0;	/* features (exp); */

	c[4] = 0;		/* sector		lba low	7:0 */
	c[5] = n;		/* cylinder low		lba mid	15:8 */
	c[6] = n >> 8;		/* cylinder hi		lba hi	23:16 */
	c[7] = Obs;

	*(ulong*)(c + 8) = 0;
	*(ulong*)(c + 12) = 0;
	*(ulong*)(c + 16) = 0;

	l->flags = 1<<16 | Lpref | Latapi | 0x5;
	if(r->write != 0 && data)
		l->flags |= Lwrite;
	l->len = 0;
	l->ctab = PCIWADDRL(t);
	l->ctabhi = PCIWADDRH(t);

	if(data == 0)
		return l;

	p = &t->prdt;
	p->dba = PCIWADDRL(data);
	p->dbahi = PCIWADDRH(data);
	p->count = 1<<31 | (n - 2) | 1;

	return l;
}

static int
waitready(Drive *d)
{
	ulong s, i, δ;

	for(i = 0; i < 15000; i += 250){
		if(d->state == Dreset || d->state == Dportreset ||
		    d->state == Dnew)
			return 1;
		δ = sys->ticks - d->lastseen;
		if(d->state == Dnull || δ > 10*1000)
			return -1;
		ilock(d);
		s = d->port->sstatus;
		iunlock(d);
		if((s & Intpm) == 0 && δ > 1500)
			return -1;	/* no detect */
		if(d->state == Dready &&
		    (s & Devdet) == (Devphycomm|Devpresent))
			return 0;	/* ready, present & phy. comm. */
		esleep(250);
	}
	print("%s: not responding; offline\n", dnam(d));
	setstate(d, Doffline);
	return -1;
}

static int
lockready(Drive *d)
{
	int i;

	qlock(&d->portm);
	while ((i = waitready(d)) == 1) {	/* could wait forever? */
		qunlock(&d->portm);
		esleep(1);
		qlock(&d->portm);
	}
	return i;
}

static int
flushcache(Drive *d)
{
	int i;

	i = -1;
	if(lockready(d) == 0)
		i = ahciflushcache(&d->portc);
	qunlock(&d->portm);
	return i;
}

static int
iariopkt(SDreq *r, Drive *d)
{
	int n, count, try, max, flag, task, wormwrite;
	char *name;
	uchar *cmd, *data;
	Aport *p;
	Asleep as;

	cmd = r->cmd;
	name = dnam(d);
	p = d->port;

	aprint("ahci: iariopkt: %04#ux %04#ux %c %d %p\n",
		cmd[0], cmd[2], "rw"[r->write], r->dlen, r->data);
	if(cmd[0] == 0x5a && (cmd[2] & 0x3f) == 0x3f)
		return sdmodesense(r, cmd, d->info, d->infosz);
	r->rlen = 0;
	count = r->dlen;
	max = 65536;

	try = 0;
retry:
	data = r->data;
	n = count;
	if(n > max)
		n = max;
	ahcibuildpkt(&d->portm, r, data, n);
	switch(waitready(d)){
	case -1:
		qunlock(&d->portm);
		return SDeio;
	case 1:
		qunlock(&d->portm);
		esleep(1);
		goto retry;
	}
	/* d->portm qlock held here */

	ilock(d);
	d->portm.flag = 0;
	iunlock(d);
	p->ci = 1;

	as.p = p;
	as.i = 1;
	d->intick = sys->ticks;
	d->active++;

	while(waserror())
		;
	/* don't sleep here forever */
	tsleep(&d->portm, ahciclear, &as, 3*1000);
	poperror();
	if(!ahciclear(&as)) {
		qunlock(&d->portm);
		print("%s: ahciclear not true after 3 seconds\n", name);
		r->status = SDcheck;
		return SDcheck;
	}

	d->active--;
	ilock(d);
	flag = d->portm.flag;
	task = d->port->task;
	iunlock(d);

	if(task & (Efatal<<8) || task & (ASbsy|ASdrq) && d->state == Dready){
		d->port->ci = 0;
		ahcirecover(&d->portc);
		task = d->port->task;
		flag &= ~Fdone;		/* either an error or do-over */
	}
	qunlock(&d->portm);
	if(flag == 0){
		if(++try == 10){
			print("%s: bad disk\n", name);
			r->status = SDcheck;
			return SDcheck;
		}
		/*
		 * write retries cannot succeed on write-once media,
		 * so just accept any failure.
		 */
		wormwrite = 0;
		switch(d->unit->inquiry[0] & SDinq0periphtype){
		case SDperworm:
		case SDpercd:
			switch(cmd[0]){
			case ScmdWrite:
			case ScmdExtwrite:
			case ScmdExtwritever:
			case ScmdWrite16:
				wormwrite = 1;
				break;
			}
			break;
		}
		if (!wormwrite) {
			print("%s: retry\n", name);
			goto retry;
		}
	}
	if(flag & Ferror){
		if((task&Eidnf) == 0)
			print("%s: i/o error task=%#ux\n", name, task);
		r->status = SDcheck;
		return SDcheck;
	}

	data += n;

	r->rlen = data - (uchar*)r->data;
	r->status = SDok;
	return SDok;
}

static int
iario(SDreq *r)
{
	int i, n, try, max, flag, task;
	ulong count;
	uvlong lba;
	char *name;
	uchar *cmd, *data;
	Aport *p;
	Asleep as;
	Ctlr *c;
	Drive *d;
	SDunit *unit;

	unit = r->unit;
	c = unit->dev->ctlr;
	d = c->drive[unit->subno];
	if(d->portm.feat & Datapi)
		return iariopkt(r, d);
	cmd = r->cmd;
	name = dnam(d);
	p = d->port;

	if(*cmd == ScmdSynccache || *cmd == ScmdSynccache16){
		if(flushcache(d) == 0)
			return sdsetsense(r, SDok, 0, 0, 0);
		return sdsetsense(r, SDcheck, 3, 0xc, 2);
	}

	if((i = sdfakescsi(r, d->info, d->infosz)) != SDnostatus){
		r->status = i;
		return i;
	}

	if(*cmd == ScmdRead16 || *cmd == ScmdWrite16){
		/* ata commands only go to 48-bit lba */
		if(cmd[2] || cmd[3])
			return sdsetsense(r, SDcheck, 3, 0xc, 2);
	}else if(*cmd != ScmdExtread && *cmd != ScmdExtwrite){
		print("%s: bad cmd %.2#ux\n", name, cmd[0]);
		r->status = SDcheck;
		return SDcheck;
	}
	scsilbacount(cmd, r->clen, &lba, &count);

	if(r->data == nil)
		return SDok;
	if(r->dlen < count * unit->secsize)
		count = r->dlen / unit->secsize;
	max = 128;
	if(d->portm.feat & Dllba){
		max = 8192;		/* ahci maximum */
		if(c->type == Tsb600)
			max = 255;	/* errata */
	}

	try = 0;
retry:
	data = r->data;
	while(count > 0){
		n = count;
		if(n > max)
			n = max;
		ahcibuild(d, cmd, data, n, lba);
		switch(waitready(d)){
		case -1:
			qunlock(&d->portm);
			return SDeio;
		case 1:
			qunlock(&d->portm);
			esleep(1);
			goto retry;
		}
		/* d->portm qlock held here */
		ilock(d);
		d->portm.flag = 0;
		iunlock(d);
		p->ci = 1;

		as.p = p;
		as.i = 1;
		d->intick = sys->ticks;
		d->active++;

		while(waserror())
			;
		/* don't sleep here forever */
		tsleep(&d->portm, ahciclear, &as, 3*1000);
		poperror();
		if(!ahciclear(&as)) {
			qunlock(&d->portm);
			print("%s: ahciclear not true after 3 seconds\n", name);
			r->status = SDcheck;
			return SDcheck;
		}

		d->active--;
		ilock(d);
		flag = d->portm.flag;
		task = d->port->task;
		iunlock(d);

		if(task & (Efatal<<8) ||
		    task & (ASbsy|ASdrq) && d->state == Dready){
			d->port->ci = 0;
			ahcirecover(&d->portc);
			task = d->port->task;
		}
		qunlock(&d->portm);
		if(flag == 0){
			if(++try == 10){
				print("%s: bad disk\n", name);
				r->status = SDeio;
				return SDeio;
			}
			print("%s: retry blk %lld\n", name, lba);
			goto retry;
		}
		if(flag & Ferror){
			print("%s: i/o error task=%#ux @%,lld\n",
				name, task, lba);
			r->status = SDeio;
			return SDeio;
		}

		count -= n;
		lba   += n;
		data += n * unit->secsize;
	}
	r->rlen = data - (uchar*)r->data;
	r->status = SDok;
	return SDok;
}

static void
iasetupahci(Ctlr *c)
{
	if(c->type != Tich)
		return;
	
	/* disable cmd block decoding. */
	pcicfgw16(c->pci, 0x40, pcicfgr16(c->pci, 0x40) & ~(1<<15));
	pcicfgw16(c->pci, 0x42, pcicfgr16(c->pci, 0x42) & ~(1<<15));

	c->lmmio[0x4/4] |= 1 << 31;	/* enable ahci mode (ghc register) */
	c->lmmio[0xc/4] = (1 << 6) - 1;	/* 6 ports. (supposedly ro pi reg.) */

	/* enable ahci mode and 6 ports; from ich9 datasheet */
	pcicfgw16(c->pci, 0x90, 1<<6 | 1<<5);

	/* configure drives 0-5 as ahci sata  (c.f. errata) */
	pcicfgw16(c->pci, 0x92, pcicfgr16(c->pci, 0x92) | 0xf);
}

static void
sbsetupahci(Pcidev *p)
{
	print("sbsetupahci: tweaking %.4ux ccru %.2ux ccrp %.2ux\n",
		p->did, p->ccru, p->ccrp);
	pcicfgw8(p, 0x40, pcicfgr8(p, 0x40) | 1);
	pcicfgw8(p, PciCCRu, Pciscsata);
	pcicfgw8(p, PciCCRp, 1);
	p->ccru = Pciscsata;
	p->ccrp = 1;
}

static int
esbenc(Ctlr *c)
{
	c->encsz = 1;
	c->enctx = (ulong*)(c->mmio + 0xa0);
	c->enctype = Eesb;
	c->enctx[0] = 0;
	return 0;
}

static int
ahciencinit(Ctlr *c)
{
	ulong type, sz, o, *bar;
	Ahba *h;

	h = c->hba;
	if(c->type == Tesb)
		return esbenc(c);
	if((h->cap & Hems) == 0)
		return -1;
	type = h->emctl & Emtype;
	switch(type){
	case Esgpio:
	case Eses2:
	case Esafte:
		return -1;
	case Elmt:
		break;
	default:
		return -1;
	}

	sz = h->emloc & 0xffff;
	o = h->emloc>>16;
	if(sz == 0 || o == 0)
		return -1;
	bar = c->lmmio;
	dprint("size = %.4lux; loc = %.4lux*4\n", sz, o);
	c->encsz = sz;
	c->enctx = bar + o;
	if((h->emctl & Xonly) == 0){
		if(h->emctl & Smb)
			c->encrx = bar + o;
		else
			c->encrx = bar + o*2;
	}
	c->enctype = type;
	return 0;
}

static int
didtype(Pcidev *p)
{
	int type;

	type = Tahci;
	switch(p->vid){
	case 0x8086:
		if((p->did & 0xfffc) == 0x2680)
			return Tesb;
		/*
		 * 0x27c4 is the intel 82801 in compatibility (not sata) mode.
		 */
		if (p->did == 0x1e02 ||			/* c210 */
		    p->did == 0x8c02 ||			/* c220 */
		    p->did == 0x24d1 ||			/* 82801eb/er */
		    p->did == 0x2653 ||			/* 82801fbm */
		    (p->did & 0xfffb) == 0x27c1 ||	/* 82801g[bh]m ich7 */
		    p->did == 0x2821 ||			/* 82801h[roh] */
		    p->did == 0x2822 ||			/* 82801 SATA RAID */
		    (p->did & 0xfffe) == 0x2824 ||	/* 82801h[b] */
		    (p->did & 0xfeff) == 0x2829 ||	/* ich8/9m */
		    (p->did & 0xfffe) == 0x2922 ||	/* ich9 */
		    p->did == 0x3a02 ||			/* 82801jd/do */
		    (p->did & 0xfefe) == 0x3a22 ||	/* ich10, pch */
		    (p->did & 0xfff7) == 0x3b28 ||	/* pchm */
		    (p->did & 0xfffe) == 0x3b22)	/* pch */
			return Tich;
		break;
	case 0x1002:
		if(p->ccru == 1 || p->ccrp != 1)
			if(p->did == 0x4380 || p->did == 0x4390 || p->did == 0x4391 || p->did == 0x4394)
				sbsetupahci(p);
		type = Tsb600;
		break;
	case 0x1106:
		/*
		 * unconfirmed report that the programming
		 * interface is set incorrectly.
		 */
		if(p->did == 0x3349)
			return Tahci;
		break;
	case 0x1022:
		/* Hudson SATA Controller [AHCI mode] */
		if((p->did & 0xfffe) == 0x7800){
			sbsetupahci(p);
			return Tahci;
		}
		break;
	case 0x10de:
	case 0x1039:
	case 0x1b21:	/* ASMedia */
	case 0x1b4b:
	case 0x11ab:
		break;
	case 0x197b:
	case 0x10b9:
		type = Tjmicron;
		break;
	}
	if(p->ccrb == Pcibcstore && p->ccru == Pciscsata && p->ccrp == 1)
		return type;
	return -1;
}

static int
newctlr(Ctlr *ctlr, SDev *sdev, int nunit)
{
	int i, n;
	ulong io;
	Drive *drive;

	ctlr->ndrive = sdev->nunit = nunit;
	ctlr->mport = ctlr->hba->cap & ((1<<5)-1);

	i = (ctlr->hba->cap >> 20) & ((1<<4)-1);		/* iss */
	print("#S/sd%c: %s: %#p %s, %d ports, irq %d\n", sdev->idno,
		Tname(ctlr), ctlr->physio, descmode[i], nunit, ctlr->pci->intl);
	/* map the drives -- they don't all need to be enabled. */
	n = 0;
	ctlr->rawdrive = malloc(NCtlrdrv * sizeof(Drive));
	if(ctlr->rawdrive == nil) {
		print("ahci: out of memory\n");
		return -1;
	}
	for(i = 0; i < NCtlrdrv; i++) {
		drive = ctlr->rawdrive + i;
		drive->portno = i;
		drive->driveno = -1;
		drive->sectors = 0;
		drive->serial[0] = ' ';
		drive->ctlr = ctlr;
		if((ctlr->hba->pi & (1<<i)) == 0)
			continue;
		io = 0x100 + 0x80*i;
		if((io + 0x80) > ctlr->pci->mem[Abar].size)
			continue;
		drive->port = (Aport*)(ctlr->mmio + io);
		drive->portc.p = drive->port;
		drive->portc.pm = &drive->portm;
		drive->driveno = n++;
		ctlr->drive[drive->driveno] = drive;
		iadrive[niadrive + drive->driveno] = drive;
	}
	pcisetbme(ctlr->pci);
	for(i = 0; i < n; i++)
		if(ahciidle(ctlr->drive[i]->port) == -1){
			dprint("ahci: %s: port %d wedged; abort\n",
				Tname(ctlr), i);
			return -1;
		}
	for(i = 0; i < n; i++){
		ctlr->drive[i]->mode = DMautoneg;
		configdrive(ctlr->drive[i]);
	}
	ahciencinit(ctlr);
	
	return n;
}

static SDev*
iapnp(void)
{
	int n, nunit, type;
	ulong io;
	Ctlr *c;
	Pcidev *p;
	SDev *head, *tail, *s;
	static int done;

	if(done++)
		return nil;

	if(getconf("*noahci") != nil)
		return nil;

	if(getconf("*ahcidebug") != nil){
		debug = 1;
		datapi = 1;
	}

	memset(olds, 0xff, sizeof olds);
	p = nil;
	head = tail = nil;
	while((p = pcimatch(p, 0, 0)) != nil){
		if((type = didtype(p)) == -1)
			continue;
		io = p->mem[Abar].bar;
		if(io == 0 || (io & 1) != 0 || p->mem[Abar].size < 0x180)
			continue;
		io &= ~0xf;
		if(niactlr == NCtlr){
			print("ahci: iapnp: %s: too many controllers\n",
				tname[type]);
			break;
		}
		c = iactlr + niactlr;
		s = sdevs  + niactlr;
		memset(c, 0, sizeof *c);
		memset(s, 0, sizeof *s);
		c->physio = (uchar *)io;
		c->mmio = vmap(io, p->mem[Abar].size);
		if(c->mmio == nil){
			print("ahci: %s: address %#luX in use did=%#x\n",
				Tname(c), io, p->did);
			continue;
		}
		c->lmmio = (ulong*)c->mmio;
		c->pci = p;
		c->type = type;

		s->ifc = &sdiahciifc;
		s->idno = 'E' + niactlr;
		s->ctlr = c;
		c->sdev = s;

		ahcihandoff((Ahba*)c->mmio);
		if(p->vid == 0x8086)
			iasetupahci(c);
		nunit = ahciconf(c);
		if(nunit < 1){
			vunmap(c->mmio, p->mem[Abar].size);
			continue;
		}
		n = newctlr(c, s, nunit);
		if(n < 0)
			continue;
		niadrive += n;
		niactlr++;
		if(head)
			tail->next = s;
		else
			head = s;
		tail = s;
	}
	return head;
}

static char* smarttab[] = {
	"unset",
	"error",
	"threshold exceeded",
	"normal"
};

static char *
pflag(char *s, char *e, uchar f)
{
	uchar i;

	for(i = 0; i < Dnflag; i++)
		if(f & (1 << i))
			s = seprint(s, e, "%s ", flagname[i]);
	return seprint(s, e, "\n");
}

static Htab ctab[] = {
	Aasp,	"asp",
	Aalpe ,	"alpe ",
	Adlae,	"dlae",
	Aatapi,	"atapi",
	Apste,	"pste",
	Afbsc,	"fbsc",
	Aesp,	"esp",
	Acpd,	"cpd",
	Ampsp,	"mpsp",
	Ahpcp,	"hpcp",
	Apma,	"pma",
	Acps,	"cps",
	Acr,	"cr",
	Afr,	"fr",
	Ampss,	"mpss",
	Apod,	"pod",
	Asud,	"sud",
	Ast,	"st",
};

static char*
capfmt(char *p, char *e, Htab *t, int n, ulong cap)
{
	uint i;

	*p = 0;
	for(i = 0; i < n; i++)
		if(cap & t[i].bit)
			p = seprint(p, e, "%s ", t[i].name);
	return p;
}

static int
iarctl(SDunit *u, char *p, int l)
{
	char buf[32];
	char *e, *op;
	Aport *o;
	Ctlr *c;
	Drive *d;

	c = u->dev->ctlr;
	if(c == nil) {
print("iarctl: nil u->dev->ctlr\n");
		return 0;
	}
	d = c->drive[u->subno];
	o = d->port;

	e = p+l;
	op = p;
	if(d->state == Dready){
		p = seprint(p, e, "model\t%s\n", d->model);
		p = seprint(p, e, "serial\t%s\n", d->serial);
		p = seprint(p, e, "firm\t%s\n", d->firmware);
		if(d->smartrs == 0xff)
			p = seprint(p, e, "smart\tenable error\n");
		else if(d->smartrs == 0)
			p = seprint(p, e, "smart\tdisabled\n");
		else
			p = seprint(p, e, "smart\t%s\n",
				smarttab[d->portm.smart]);
		p = seprint(p, e, "flag\t");
		p = pflag(p, e, d->portm.feat);
		p = seprint(p, e, "udma\t%d\n", d->portm.udma);
	}else
		p = seprint(p, e, "no disk present [%s]\n", diskstates[d->state]);
	serrstr(o->serror, buf, buf + sizeof buf - 1);
	p = seprint(p, e, "reg\ttask %#lux cmd %#lux serr %#lux %s ci %#lux "
		"is %#lux; sig %#lux sstatus %06#lux\n",
		o->task, o->cmd, o->serror, buf,
		o->ci, o->isr, o->sig, o->sstatus);
	p = seprint(p, e, "cmd\t");
	p = capfmt(p, e, ctab, nelem(ctab), o->cmd);
	p = seprint(p, e, "\n");
	p = seprint(p, e, "mode\t%s %s\n", modename[d->mode], modename[maxmode(c)]);
	if(d->unit == nil)
		panic("iarctl: nil d->unit");
	p = seprint(p, e, "geometry %llud %lud\n", d->sectors, d->unit->secsize);
	return p - op;
}

static void
runflushcache(Drive *d)
{
	long t0;

	t0 = sys->ticks;
	if(flushcache(d) != 0)
		error(Eio);
	dprint("ahci: flush in %ld ms\n", sys->ticks - t0);
}

static void
forcemode(Drive *d, char *mode)
{
	int i;

	for(i = 0; i < nelem(modename); i++)
		if(strcmp(mode, modename[i]) == 0)
			break;
	if(i == nelem(modename))
		i = 0;
	ilock(d);
	d->mode = i;
	iunlock(d);
}

static void
runsmartable(Drive *d, int i)
{
	if(waserror()){
		qunlock(&d->portm);
		d->smartrs = 0;
		nexterror();
	}
	if(lockready(d) == -1)
		error(Eio);
	d->smartrs = smart(&d->portc, i);
	d->portm.smart = 0;
	qunlock(&d->portm);
	poperror();
}

static void
forcestate(Drive *d, char *state)
{
	int i;

	for(i = 0; i < nelem(diskstates); i++)
		if(strcmp(state, diskstates[i]) == 0)
			break;
	if(i == nelem(diskstates))
		error(Ebadctl);
	setstate(d, i);
}

/*
 * force this driver to notice a change of medium if the hardware doesn't
 * report it.
 */
static void
changemedia(SDunit *u)
{
	Ctlr *c;
	Drive *d;

	c = u->dev->ctlr;
	d = c->drive[u->subno];
	ilock(d);
	d->mediachange = 1;
	u->sectors = 0;
	iunlock(d);
}

static int
runsetudmamode(Drive *d, char *s)
{
	int i;
	Aportc *pc;
	Aportm *pm;

	pc = &d->portc;
	pm = &d->portm;

	i = 1;
	if(lockready(d) == 0){
		pm->udma = atoi(s);
		if(setudmamode(pc, pm->udma) == 0)
			i = 0;
	}
	qunlock(pm);
	return i;
}

static int
iawctl(SDunit *u, Cmdbuf *cmd)
{
	char **f;
	Ctlr *c;
	Drive *d;
	uint i;

	c = u->dev->ctlr;
	d = c->drive[u->subno];
	f = cmd->f;

	if(strcmp(f[0], "change") == 0)
		changemedia(u);
	else if(strcmp(f[0], "flushcache") == 0)
		runflushcache(d);
	else if(strcmp(f[0], "identify") ==  0){
		i = strtoul(f[1]? f[1]: "0", 0, 0);
		if(i > 0xff)
			i = 0;
		dprint("ahci: %04d %#ux\n", i, d->info[i]);
	}else if(strcmp(f[0], "mode") == 0)
		forcemode(d, f[1]? f[1]: "satai");
	else if(strcmp(f[0], "nop") == 0){
		if((d->portm.feat & Dnop) == 0){
			cmderror(cmd, "no drive support");
			return -1;
		}
		if(waserror()){
			qunlock(&d->portm);
			nexterror();
		}
		if(lockready(d) == -1)
			error(Eio);
		nop(&d->portc);
		qunlock(&d->portm);
		poperror();
	}else if(strcmp(f[0], "reset") == 0)
		forcestate(d, "reset");
	else if(strcmp(f[0], "smart") == 0){
		if(d->smartrs == 0){
			cmderror(cmd, "smart not enabled");
			return -1;
		}
		if(waserror()){
			qunlock(&d->portm);
			d->smartrs = 0;
			nexterror();
		}
		if(lockready(d) == -1)
			error(Eio);
		d->portm.smart = 2 + smartrs(&d->portc);
		qunlock(&d->portm);
		poperror();
	}else if(strcmp(f[0], "smartdisable") == 0)
		runsmartable(d, 1);
	else if(strcmp(f[0], "smartenable") == 0)
		runsmartable(d, 0);
	else if(strcmp(f[0], "state") == 0)
		forcestate(d, f[1]? f[1]: "null");
	else if(strcmp(f[0], "udmamode") == 0){
		if(runsetudmamode(d, f[1]? f[1]: "0"))
			cmderror(cmd, "bad udmamode / stuck port");
	}else{
		cmderror(cmd, Ebadctl);
		return -1;
	}
	return 0;
}

static char *
portr(char *p, char *e, uint x)
{
	int i, a;

	p[0] = 0;
	a = -1;
	for(i = 0; i < 32; i++){
		if((x & (1<<i)) == 0){
			if(a != -1 && i - 1 != a)
				p = seprint(p, e, "-%d", i - 1);
			a = -1;
			continue;
		}
		if(a == -1){
			if(i > 0)
				p = seprint(p, e, ", ");
			p = seprint(p, e, "%d", a = i);
		}
	}
	if(a != -1 && i - 1 != a)
		p = seprint(p, e, "-%d", i - 1);
	return p;
}

/* must emit exactly one line per controller (sd(3)) */
static char*
iartopctl(SDev *sdev, char *p, char *e)
{
	ulong cap;
	char pr[25];
	Ahba *hba;
	Ctlr *ctlr;

#define has(x, str) if(cap & (x)) p = seprint(p, e, "%s ", (str))

	ctlr = sdev->ctlr;
	hba = ctlr->hba;
	p = seprint(p, e, "sd%c ahci port %#p: ", sdev->idno, ctlr->physio);
	cap = hba->cap;
	has(Hs64a, "64a");
	has(Hsalp, "alp");
	has(Hsam, "am");
	has(Hsclo, "clo");
	has(Hcccs, "coal");
	has(Hems, "ems");
	has(Hsal, "led");
	has(Hsmps, "mps");
	has(Hsncq, "ncq");
	has(Hssntf, "ntf");
	has(Hspm, "pm");
	has(Hpsc, "pslum");
	has(Hssc, "slum");
	has(Hsss, "ss");
	has(Hsxs, "sxs");
	portr(pr, pr + sizeof pr, hba->pi);
	return seprint(p, e,
		"iss %ld ncs %ld np %ld; ghc %#lux isr %#lux pi %#lux %s ver %#lux\n",
		(cap>>20) & 0xf, (cap>>8) & 0x1f, 1 + (cap & 0x1f),
		hba->ghc, hba->isr, hba->pi, pr, hba->ver);
#undef has
}

static int
iawtopctl(SDev *, Cmdbuf *cmd)
{
	int *v;
	char **f;

	f = cmd->f;
	v = 0;

	if (f[0] == nil)
		return 0;
	if(strcmp(f[0], "debug") == 0)
		v = &debug;
	else if(strcmp(f[0], "idprint") == 0)
		v = &prid;
	else if(strcmp(f[0], "aprint") == 0)
		v = &datapi;
	else
		cmderror(cmd, Ebadctl);

	switch(cmd->nf){
	default:
		cmderror(cmd, Ebadarg);
	case 1:
		*v ^= 1;
		break;
	case 2:
		if(f[1])
			*v = strcmp(f[1], "on") == 0;
		else
			*v ^= 1;
		break;
	}
	return 0;
}

SDifc sdiahciifc = {
	"iahci",

	iapnp,
	nil,		/* legacy */
	iaenable,
	iadisable,

	iaverify,
	iaonline,
	iario,
	iarctl,
	iawctl,

	scsibio,
	nil,		/* probe */
	nil,		/* clear */
	iartopctl,
	iawtopctl,
};
