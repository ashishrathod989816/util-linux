/*
 *
 * Copyright (C) 2007-2013 Karel Zak <kzak@redhat.com>
 *                    2012 Davidlohr Bueso <dave@gnu.org>
 *
 * This is re-written version for libfdisk, the original was fdiskdoslabel.c
 * from util-linux fdisk.
 */
#include "c.h"
#include "nls.h"
#include "randutils.h"
#include "pt-mbr.h"
#include "strutils.h"

#include "fdiskP.h"

#include <ctype.h>

#define MAXIMUM_PARTS	60
#define ACTIVE_FLAG     0x80

#define IS_EXTENDED(i) \
	((i) == MBR_DOS_EXTENDED_PARTITION \
	 || (i) == MBR_W95_EXTENDED_PARTITION \
	 || (i) == MBR_LINUX_EXTENDED_PARTITION)

/*
 * per partition table entry data
 *
 * The four primary partitions have the same sectorbuffer
 * and have NULL ex_entry.
 *
 * Each logical partition table entry has two pointers, one for the
 * partition and one link to the next one.
 */
struct pte {
	struct dos_partition *pt_entry;	/* on-disk MBR entry */
	struct dos_partition *ex_entry;	/* on-disk EBR entry */
	sector_t offset;	        /* disk sector number */
	unsigned char *sectorbuffer;	/* disk sector contents */

	unsigned int changed : 1,
		     private_sectorbuffer : 1;
};

/*
 * in-memory fdisk GPT stuff
 */
struct fdisk_dos_label {
	struct fdisk_label	head;		/* generic part */

	struct pte	ptes[MAXIMUM_PARTS];	/* partition */
	sector_t	ext_offset;		/* start of the ext.partition */
	size_t		ext_index;		/* ext.partition index (if ext_offset is set) */
	unsigned int	compatible : 1,		/* is DOS compatible? */
			non_pt_changed : 1;	/* MBR, but no PT changed */
};

/*
 * Partition types
 */
static struct fdisk_parttype dos_parttypes[] = {
	#include "pt-mbr-partnames.h"
};

#define set_hsc(h,s,c,sector) { \
		s = sector % cxt->geom.sectors + 1;			\
		sector /= cxt->geom.sectors;				\
		h = sector % cxt->geom.heads;				\
		sector /= cxt->geom.heads;				\
		c = sector & 0xff;					\
		s |= (sector >> 2) & 0xc0;				\
	}


#define sector(s)	((s) & 0x3f)
#define cylinder(s, c)	((c) | (((s) & 0xc0) << 2))

#define alignment_required(_x)	((_x)->grain != (_x)->sector_size)

#define is_dos_compatible(_x) \
		   (fdisk_is_disklabel(_x, DOS) && \
                    fdisk_dos_is_compatible(fdisk_context_get_label(_x, NULL)))

#define cround(c, n)	fdisk_cround(c, n)


static inline struct fdisk_dos_label *self_label(struct fdisk_context *cxt)
{
	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	return (struct fdisk_dos_label *) cxt->label;
}

static inline struct pte *self_pte(struct fdisk_context *cxt, size_t i)
{
	struct fdisk_dos_label *l = self_label(cxt);

	if (i >= ARRAY_SIZE(l->ptes))
		return NULL;

	return &l->ptes[i];
}

static inline struct dos_partition *self_partition(
				struct fdisk_context *cxt,
				size_t i)
{
	struct pte *pe = self_pte(cxt, i);
	return pe ? pe->pt_entry : NULL;
}

struct dos_partition *fdisk_dos_get_partition(
				struct fdisk_context *cxt,
				size_t i)
{
	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	return self_partition(cxt, i);
}

static struct fdisk_parttype *dos_partition_parttype(
		struct fdisk_context *cxt,
		struct dos_partition *p)
{
	struct fdisk_parttype *t
			= fdisk_get_parttype_from_code(cxt, p->sys_ind);
	return t ? : fdisk_new_unknown_parttype(p->sys_ind, NULL);
}

/*
 * Linux kernel cares about partition size only. Things like
 * partition type or so are completely irrelevant -- kzak Nov-2013
 */
static int is_used_partition(struct dos_partition *p)
{
	return p && dos_partition_get_size(p) != 0;
}

static void partition_set_changed(
				struct fdisk_context *cxt,
				size_t i,
				int changed)
{
	struct pte *pe = self_pte(cxt, i);

	if (!pe)
		return;

	DBG(LABEL, dbgprint("DOS: setting %zu partition to %s", i,
				changed ? "changed" : "unchnaged"));

	pe->changed = changed ? 1 : 0;
	if (changed)
		fdisk_label_set_changed(cxt->label, 1);
}

static sector_t get_abs_partition_start(struct pte *pe)
{
	assert(pe);
	assert(pe->pt_entry);

	return pe->offset + dos_partition_get_start(pe->pt_entry);
}

static sector_t get_abs_partition_end(struct pte *pe)
{
	sector_t size;

	assert(pe);
	assert(pe->pt_entry);

	size = dos_partition_get_size(pe->pt_entry);
	return get_abs_partition_start(pe) + size - (size ? 1 : 0);
}

static int is_cleared_partition(struct dos_partition *p)
{
	return !(!p || p->boot_ind || p->bh || p->bs || p->bc ||
		 p->sys_ind || p->eh || p->es || p->ec ||
		 dos_partition_get_start(p) || dos_partition_get_size(p));
}

static int get_partition_unused_primary(struct fdisk_context *cxt,
					struct fdisk_partition *pa)
{
	size_t org = cxt->label->nparts_max, n;
	int rc;

	cxt->label->nparts_max = 4;
	rc = fdisk_partition_next_partno(pa, cxt, &n);
	cxt->label->nparts_max = org;

	switch (rc) {
	case 1:
		fdisk_info(cxt, _("All primary partitions have been defined already."));
		return -1;
	case 0:
		return n;
	default:
		return rc;
	}
}

static int seek_sector(struct fdisk_context *cxt, sector_t secno)
{
	off_t offset = (off_t) secno * cxt->sector_size;

	return lseek(cxt->dev_fd, offset, SEEK_SET) == (off_t) -1 ? -errno : 0;
}

static int read_sector(struct fdisk_context *cxt, sector_t secno,
			unsigned char *buf)
{
	int rc = seek_sector(cxt, secno);

	if (rc < 0)
		return rc;

	return read(cxt->dev_fd, buf, cxt->sector_size) !=
			(ssize_t) cxt->sector_size ? -errno : 0;
}

/* Allocate a buffer and read a partition table sector */
static int read_pte(struct fdisk_context *cxt, size_t pno, sector_t offset)
{
	unsigned char *buf;
	struct pte *pe = self_pte(cxt, pno);

	buf = calloc(1, cxt->sector_size);
	if (!buf)
		return -ENOMEM;

	DBG(LABEL, dbgprint("DOS: reading pte %zu sector buffer %p", pno, buf));

	pe->offset = offset;
	pe->sectorbuffer = buf;
	pe->private_sectorbuffer = 1;

	if (read_sector(cxt, offset, pe->sectorbuffer) != 0)
		fdisk_warn(cxt, _("Failed to read extended partition table "
				"(offset=%ju)"), (uintmax_t) offset);
	pe->changed = 0;
	pe->pt_entry = pe->ex_entry = NULL;
	return 0;
}


static void clear_partition(struct dos_partition *p)
{
	if (!p)
		return;
	p->boot_ind = 0;
	p->bh = 0;
	p->bs = 0;
	p->bc = 0;
	p->sys_ind = 0;
	p->eh = 0;
	p->es = 0;
	p->ec = 0;
	dos_partition_set_start(p,0);
	dos_partition_set_size(p,0);
}

static void dos_init(struct fdisk_context *cxt)
{
	struct fdisk_dos_label *l = self_label(cxt);
	size_t i;

	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	DBG(LABEL, dbgprint("DOS: initialize, first sector buffer %p", cxt->firstsector));

	cxt->label->nparts_max = 4;	/* default, unlimited number of logical */

	l->ext_index = 0;
	l->ext_offset = 0;
	l->non_pt_changed = 0;

	memset(l->ptes, 0, sizeof(l->ptes));

	for (i = 0; i < 4; i++) {
		struct pte *pe = self_pte(cxt, i);

		pe->pt_entry = mbr_get_partition(cxt->firstsector, i);
		pe->ex_entry = NULL;
		pe->offset = 0;
		pe->sectorbuffer = cxt->firstsector;
		pe->private_sectorbuffer = 0;
		pe->changed = 0;
	}

	if (fdisk_context_listonly(cxt))
		return;
	/*
	 * Various warnings...
	 */
	if (fdisk_missing_geometry(cxt))
		fdisk_warnx(cxt, _("You can set geometry from the extra functions menu."));

	if (is_dos_compatible(cxt)) {
		fdisk_warnx(cxt, _("DOS-compatible mode is deprecated."));

		if (cxt->sector_size != cxt->phy_sector_size)
			fdisk_info(cxt, _(
		"The device presents a logical sector size that is smaller than "
		"the physical sector size. Aligning to a physical sector (or optimal "
		"I/O) size boundary is recommended, or performance may be impacted."));
	}

	if (fdisk_context_use_cylinders(cxt))
		fdisk_warnx(cxt, _("Cylinders as display units are deprecated."));

	if (cxt->total_sectors > UINT_MAX) {
		uint64_t bytes = cxt->total_sectors * cxt->sector_size;
		char *szstr = size_to_human_string(SIZE_SUFFIX_SPACE
					   | SIZE_SUFFIX_3LETTER, bytes);
		fdisk_warnx(cxt,
		_("The size of this disk is %s (%ju bytes). DOS "
		  "partition table format can not be used on drives for "
		  "volumes larger than %ju bytes for %lu-byte "
		  "sectors. Use GUID partition table format (GPT)."),
			szstr, bytes,
			UINT_MAX * cxt->sector_size,
			cxt->sector_size);
		free(szstr);
	}
}

/* callback called by libfdisk */
static void dos_deinit(struct fdisk_label *lb)
{
	size_t i;
	struct fdisk_dos_label *l = (struct fdisk_dos_label *) lb;

	for (i = 0; i < ARRAY_SIZE(l->ptes); i++) {
		struct pte *pe = &l->ptes[i];

		if (pe->private_sectorbuffer && pe->sectorbuffer) {
			DBG(LABEL, dbgprint("DOS: freeing pte %zu sector buffer %p",
						i, pe->sectorbuffer));
			free(pe->sectorbuffer);
		}
		pe->sectorbuffer = NULL;
		pe->private_sectorbuffer = 0;
	}

	memset(l->ptes, 0, sizeof(l->ptes));
}

static int dos_delete_partition(struct fdisk_context *cxt, size_t partnum)
{
	struct fdisk_dos_label *l;
	struct pte *pe;
	struct dos_partition *p;
	struct dos_partition *q;

	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	pe = self_pte(cxt, partnum);
	if (!pe)
		return -EINVAL;

	DBG(LABEL, dbgprint("DOS: delete partiton %zu (max=%zu)", partnum,
				cxt->label->nparts_max));

	l = self_label(cxt);
	p = pe->pt_entry;
	q = pe->ex_entry;

	/* Note that for the fifth partition (partnum == 4) we don't actually
	   decrement partitions. */
	if (partnum < 4) {
		DBG(LABEL, dbgprint("--> delete primary"));
		if (IS_EXTENDED(p->sys_ind) && partnum == l->ext_index) {
			cxt->label->nparts_max = 4;
			l->ptes[l->ext_index].ex_entry = NULL;
			l->ext_offset = 0;
			l->ext_index = 0;
		}
		partition_set_changed(cxt, partnum, 1);
		clear_partition(p);
	} else if (!q->sys_ind && partnum > 4) {
		DBG(LABEL, dbgprint("--> delete logical [last in the chain]"));
		--cxt->label->nparts_max;
		--partnum;
		clear_partition(l->ptes[partnum].ex_entry);
		partition_set_changed(cxt, partnum, 1);
	} else {
		DBG(LABEL, dbgprint("--> delete logical [move down]"));
		if (partnum > 4) {
			DBG(LABEL, dbgprint(" --> delete %zu logical link", partnum));
			p = l->ptes[partnum - 1].ex_entry;
			*p = *q;
			dos_partition_set_start(p, dos_partition_get_start(q));
			dos_partition_set_size(p, dos_partition_get_size(q));
			partition_set_changed(cxt, partnum - 1, 1);

		} else if (cxt->label->nparts_max > 5) {
			DBG(LABEL, dbgprint(" --> delete first logical link"));
			pe = &l->ptes[5];	/* second logical */

			if (pe->pt_entry)	/* prevent SEGFAULT */
				dos_partition_set_start(pe->pt_entry,
					       get_abs_partition_start(pe) -
					       l->ext_offset);
			pe->offset = l->ext_offset;
			partition_set_changed(cxt, 5, 1);
		}

		if (cxt->label->nparts_max > 5) {
			DBG(LABEL, dbgprint(" --> move ptes"));
			cxt->label->nparts_max--;
			if (l->ptes[partnum].private_sectorbuffer) {
				DBG(LABEL, dbgprint("  --> freeing pte %zu sector buffer %p",
							partnum, l->ptes[partnum].sectorbuffer));
				free(l->ptes[partnum].sectorbuffer);
			}
			while (partnum < cxt->label->nparts_max) {
				DBG(LABEL, dbgprint("  --> moving pte %zu <-- %zu", partnum, partnum + 1));
				l->ptes[partnum] = l->ptes[partnum + 1];
				partnum++;
			}
			memset(&l->ptes[partnum], 0, sizeof(struct pte));
		} else {
			DBG(LABEL, dbgprint(" --> the only logical: clear only"));
			clear_partition(l->ptes[partnum].pt_entry);
			cxt->label->nparts_max--;

			if (partnum == 4) {
				DBG(LABEL, dbgprint("  --> clear last logical"));
				if (l->ptes[partnum].private_sectorbuffer) {
					DBG(LABEL, dbgprint("  --> freeing pte %zu sector buffer %p",
							partnum, l->ptes[partnum].sectorbuffer));
					free(l->ptes[partnum].sectorbuffer);
				}
				memset(&l->ptes[partnum], 0, sizeof(struct pte));
				partition_set_changed(cxt, l->ext_index, 1);
			}
		}
	}

	fdisk_label_set_changed(cxt->label, 1);
	return 0;
}

static void read_extended(struct fdisk_context *cxt, size_t ext)
{
	size_t i;
	struct pte *pex;
	struct dos_partition *p, *q;
	struct fdisk_dos_label *l = self_label(cxt);

	l->ext_index = ext;
	pex = self_pte(cxt, ext);
	pex->ex_entry = pex->pt_entry;

	p = pex->pt_entry;
	if (!dos_partition_get_start(p)) {
		fdisk_warnx(cxt, _("Bad offset in primary extended partition."));
		return;
	}

	DBG(LABEL, dbgprint("DOS: Reading extended %zu", ext));

	while (IS_EXTENDED (p->sys_ind)) {
		struct pte *pe = self_pte(cxt, cxt->label->nparts_max);

		if (cxt->label->nparts_max >= MAXIMUM_PARTS) {
			/* This is not a Linux restriction, but
			   this program uses arrays of size MAXIMUM_PARTS.
			   Do not try to `improve' this test. */
			struct pte *pre = self_pte(cxt,
						cxt->label->nparts_max - 1);
			fdisk_warnx(cxt,
			_("Omitting partitions after #%zu. They will be deleted "
			  "if you save this partition table."),
				cxt->label->nparts_max);

			clear_partition(pre->ex_entry);
			partition_set_changed(cxt,
					cxt->label->nparts_max - 1, 1);
			return;
		}

		read_pte(cxt, cxt->label->nparts_max,
			 l->ext_offset + dos_partition_get_start(p));

		if (!l->ext_offset)
			l->ext_offset = dos_partition_get_start(p);

		q = p = mbr_get_partition(pe->sectorbuffer, 0);

		for (i = 0; i < 4; i++, p++) {
			if (!dos_partition_get_size(p))
				continue;

			if (IS_EXTENDED (p->sys_ind)) {
				if (pe->ex_entry)
					fdisk_warnx(cxt, _(
					"Extra link pointer in partition "
					"table %zu."),
						cxt->label->nparts_max + 1);
				else
					pe->ex_entry = p;
			} else if (p->sys_ind) {
				if (pe->pt_entry)
					fdisk_warnx(cxt, _(
					"Ignoring extra data in partition "
					"table %zu."),
						cxt->label->nparts_max + 1);
				else
					pe->pt_entry = p;
			}
		}

		/* very strange code here... */
		if (!pe->pt_entry) {
			if (q != pe->ex_entry)
				pe->pt_entry = q;
			else
				pe->pt_entry = q + 1;
		}
		if (!pe->ex_entry) {
			if (q != pe->pt_entry)
				pe->ex_entry = q;
			else
				pe->ex_entry = q + 1;
		}

		p = pe->ex_entry;
		cxt->label->nparts_cur = ++cxt->label->nparts_max;
	}

	/* remove empty links */
 remove:
	q = self_partition(cxt, 4);
	for (i = 4; i < cxt->label->nparts_max; i++) {
		p = self_partition(cxt, i);

		if (!dos_partition_get_size(p) &&
		    (cxt->label->nparts_max > 5 || q->sys_ind)) {
			fdisk_info(cxt, _("omitting empty partition (%zu)"), i+1);
			dos_delete_partition(cxt, i);
			goto remove; 	/* numbering changed */
		}
	}
}

static int dos_get_disklabel_id(struct fdisk_context *cxt, char **id)
{
	unsigned int num;

	assert(cxt);
	assert(id);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	num = mbr_get_id(cxt->firstsector);
	if (asprintf(id, "0x%08x", num) > 0)
		return 0;

	return -ENOMEM;
}

static int dos_create_disklabel(struct fdisk_context *cxt)
{
	unsigned int id;

	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	DBG(LABEL, dbgprint("DOS: creating new disklabel"));

	/* random disk signature */
	random_get_bytes(&id, sizeof(id));

	dos_init(cxt);
	fdisk_zeroize_firstsector(cxt);
	fdisk_label_set_changed(cxt->label, 1);

	/* Generate an MBR ID for this disk */
	mbr_set_id(cxt->firstsector, id);

	/* Put MBR signature */
	mbr_set_magic(cxt->firstsector);

	fdisk_sinfo(cxt, FDISK_INFO_SUCCESS,
			("Created a new DOS disklabel with disk "
			 "identifier 0x%08x."), id);
	return 0;
}

static int dos_set_disklabel_id(struct fdisk_context *cxt)
{
	char *end = NULL, *str = NULL;
	unsigned int id, old;
	struct fdisk_dos_label *l;
	int rc;

	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	DBG(LABEL, dbgprint("DOS: setting Id"));

	l = self_label(cxt);
	old = mbr_get_id(cxt->firstsector);
	rc = fdisk_ask_string(cxt,
			_("Enter the new disk identifier"), &str);
	if (rc)
		return rc;

	errno = 0;
	id = strtoul(str, &end, 0);
	if (errno || str == end || (end && *end)) {
		fdisk_warnx(cxt, _("Incorrect value."));
		return -EINVAL;
	}


	mbr_set_id(cxt->firstsector, id);
	l->non_pt_changed = 1;
	fdisk_label_set_changed(cxt->label, 1);

	fdisk_sinfo(cxt, FDISK_INFO_SUCCESS,
			_("Disk identifier changed from 0x%08x to 0x%08x."),
			old, id);
	return 0;
}

static void get_partition_table_geometry(struct fdisk_context *cxt,
			unsigned int *ph, unsigned int *ps)
{
	unsigned char *bufp = cxt->firstsector;
	struct dos_partition *p;
	int i, h, s, hh, ss;
	int first = 1;
	int bad = 0;

	hh = ss = 0;
	for (i = 0; i < 4; i++) {
		p = mbr_get_partition(bufp, i);
		if (p->sys_ind != 0) {
			h = p->eh + 1;
			s = (p->es & 077);
			if (first) {
				hh = h;
				ss = s;
				first = 0;
			} else if (hh != h || ss != s)
				bad = 1;
		}
	}

	if (!first && !bad) {
		*ph = hh;
		*ps = ss;
	}

	DBG(LABEL, dbgprint("DOS PT geometry: heads=%u, sectors=%u", *ph, *ps));
}

static int dos_reset_alignment(struct fdisk_context *cxt)
{
	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	/* overwrite necessary stuff by DOS deprecated stuff */
	if (is_dos_compatible(cxt)) {
		DBG(LABEL, dbgprint("DOS: reseting alignemnt for DOS-comaptiblem PT"));
		if (cxt->geom.sectors)
			cxt->first_lba = cxt->geom.sectors;	/* usually 63 */

		cxt->grain = cxt->sector_size;			/* usually 512 */
	}

	return 0;
}

/* TODO: move to include/pt-dos.h and share with libblkid */
#define AIX_MAGIC_STRING	"\xC9\xC2\xD4\xC1"
#define AIX_MAGIC_STRLEN	(sizeof(AIX_MAGIC_STRING) - 1)

static int dos_probe_label(struct fdisk_context *cxt)
{
	size_t i;
	unsigned int h = 0, s = 0;

	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	/* ignore disks with AIX magic number */
	if (memcmp(cxt->firstsector, AIX_MAGIC_STRING, AIX_MAGIC_STRLEN) == 0)
		return 0;

	if (!mbr_is_valid_magic(cxt->firstsector))
		return 0;

	dos_init(cxt);

	get_partition_table_geometry(cxt, &h, &s);
	if (h && s) {
		cxt->geom.heads = h;
	        cxt->geom.sectors = s;
	}

	for (i = 0; i < 4; i++) {
		struct pte *pe = self_pte(cxt, i);

		if (is_used_partition(pe->pt_entry))
			cxt->label->nparts_cur++;

		if (IS_EXTENDED (pe->pt_entry->sys_ind)) {
			if (cxt->label->nparts_max != 4)
				fdisk_warnx(cxt, _(
				"Ignoring extra extended partition %zu"),
					i + 1);
			else
				read_extended(cxt, i);
		}
	}

	for (i = 3; i < cxt->label->nparts_max; i++) {
		struct pte *pe = self_pte(cxt, i);
		struct fdisk_dos_label *l = self_label(cxt);

		if (!mbr_is_valid_magic(pe->sectorbuffer)) {
			fdisk_info(cxt, _(
			"Invalid flag 0x%02x%02x of EBR (for partition %zu) will "
			"be corrected by w(rite)."),
				pe->sectorbuffer[510],
				pe->sectorbuffer[511],
				i + 1);
			partition_set_changed(cxt, i, 1);

			/* mark also extended as changed to update the first EBR
			 * in situation that there is no logical partitions at all */
			partition_set_changed(cxt, l->ext_index, 1);
		}
	}

	return 1;
}

/*
 * Avoid warning about DOS partitions when no DOS partition was changed.
 * Here a heuristic "is probably dos partition".
 * We might also do the opposite and warn in all cases except
 * for "is probably nondos partition".
 */
static int is_dos_partition(int t)
{
	return (t == 1 || t == 4 || t == 6 ||
		t == 0x0b || t == 0x0c || t == 0x0e ||
		t == 0x11 || t == 0x12 || t == 0x14 || t == 0x16 ||
		t == 0x1b || t == 0x1c || t == 0x1e || t == 0x24 ||
		t == 0xc1 || t == 0xc4 || t == 0xc6);
}

static void set_partition(struct fdisk_context *cxt,
			  int i, int doext, sector_t start,
			  sector_t stop, int sysid)
{
	struct pte *pe = self_pte(cxt, i);
	struct dos_partition *p;
	sector_t offset;

	DBG(LABEL, dbgprint("DOS: setting partition %d%s, start=%zu, stop=%zu, sysid=%02x",
				i, doext ? " [extended]" : "",
				(size_t) start, (size_t) stop, sysid));

	if (doext) {
		struct fdisk_dos_label *l = self_label(cxt);
		p = pe->ex_entry;
		offset = l->ext_offset;
	} else {
		p = pe->pt_entry;
		offset = pe->offset;
	}
	p->boot_ind = 0;
	p->sys_ind = sysid;
	dos_partition_set_start(p, start - offset);
	dos_partition_set_size(p, stop - start + 1);

	if (!doext) {
		struct fdisk_parttype *t = fdisk_get_parttype_from_code(cxt, sysid);
		fdisk_info_new_partition(cxt, i + 1, start, stop, t);
	}
	if (is_dos_compatible(cxt) && (start/(cxt->geom.sectors*cxt->geom.heads) > 1023))
		start = cxt->geom.heads*cxt->geom.sectors*1024 - 1;
	set_hsc(p->bh, p->bs, p->bc, start);
	if (is_dos_compatible(cxt) && (stop/(cxt->geom.sectors*cxt->geom.heads) > 1023))
		stop = cxt->geom.heads*cxt->geom.sectors*1024 - 1;
	set_hsc(p->eh, p->es, p->ec, stop);
	partition_set_changed(cxt, i, 1);
}

static sector_t get_unused_start(struct fdisk_context *cxt,
				 int part_n, sector_t start,
				 sector_t first[], sector_t last[])
{
	size_t i;

	for (i = 0; i < cxt->label->nparts_max; i++) {
		sector_t lastplusoff;
		struct pte *pe = self_pte(cxt, i);

		if (start == pe->offset)
			start += cxt->first_lba;
		lastplusoff = last[i] + ((part_n < 4) ? 0 : cxt->first_lba);
		if (start >= first[i] && start <= lastplusoff)
			start = lastplusoff + 1;
	}

	return start;
}

static void fill_bounds(struct fdisk_context *cxt,
			sector_t *first, sector_t *last)
{
	size_t i;
	struct pte *pe = self_pte(cxt, 0);
	struct dos_partition *p;

	for (i = 0; i < cxt->label->nparts_max; pe++,i++) {
		p = pe->pt_entry;
		if (is_cleared_partition(p) || IS_EXTENDED (p->sys_ind)) {
			first[i] = 0xffffffff;
			last[i] = 0;
		} else {
			first[i] = get_abs_partition_start(pe);
			last[i]  = get_abs_partition_end(pe);
		}
	}
}

static int get_start_from_user(	struct fdisk_context *cxt,
				sector_t *start,
				sector_t low,
				sector_t dflt,
				sector_t limit,
				struct fdisk_partition *pa)
{
	assert(start);

	/* try to use tepmlate from 'pa' */
	if (pa && pa->start_follow_default)
		*start = dflt;

	else if (pa && pa->start) {
		DBG(LABEL, dbgprint("DOS: start: wanted=%ju, low=%ju, limit=%ju",
				(uintmax_t) pa->start, (uintmax_t) low, (uintmax_t) limit));
		*start = pa->start;
		if (*start < low || *start > limit) {
			fdisk_warnx(cxt, _("Start sector %ju out of range."),
					(uintmax_t) *start);
			return -ERANGE;
		}
	} else {
		/* ask user by dialog */
		struct fdisk_ask *ask = fdisk_new_ask();
		int rc;

		if (!ask)
			return -ENOMEM;
		fdisk_ask_set_query(ask,
			fdisk_context_use_cylinders(cxt) ?
				_("First cylinder") : _("First sector"));
		fdisk_ask_set_type(ask, FDISK_ASKTYPE_NUMBER);
		fdisk_ask_number_set_low(ask, fdisk_cround(cxt, low));
		fdisk_ask_number_set_default(ask, fdisk_cround(cxt, dflt));
		fdisk_ask_number_set_high(ask, fdisk_cround(cxt, limit));

		rc = fdisk_do_ask(cxt, ask);
		*start = fdisk_ask_number_get_result(ask);
		fdisk_free_ask(ask);
		if (rc)
			return rc;
		if (fdisk_context_use_cylinders(cxt)) {
		        *start = (*start - 1)
				* fdisk_context_get_units_per_sector(cxt);
			if (*start < low)
				*start = low;
		}
	}

	return 0;
}

static int add_partition(struct fdisk_context *cxt, size_t n,
			 struct fdisk_partition *pa)
{
	int sys, read = 0, rc, isrel = 0;
	size_t i;
	struct fdisk_dos_label *l = self_label(cxt);
	struct dos_partition *p = self_partition(cxt, n);
	struct pte *ext_pe = l->ext_offset ? self_pte(cxt, l->ext_index) : NULL;

	sector_t start, stop = 0, limit, temp,
		first[cxt->label->nparts_max],
		last[cxt->label->nparts_max];

	DBG(LABEL, dbgprint("DOS: adding partition %zu", n));

	sys = pa && pa->type ? pa->type->type : MBR_LINUX_DATA_PARTITION;

	if (is_used_partition(p)) {
		fdisk_warnx(cxt, _("Partition %zu is already defined.  "
			           "Delete it before re-adding it."),
				n + 1);
		return -EINVAL;
	}
	fill_bounds(cxt, first, last);
	if (n < 4) {
		if (cxt->parent && fdisk_is_disklabel(cxt->parent, GPT))
			start = 1;		/* Bad boy modifies hybrid MBR */
		else
			start = cxt->first_lba;

		if (fdisk_context_use_cylinders(cxt) || !cxt->total_sectors)
			limit = cxt->geom.heads * cxt->geom.sectors * cxt->geom.cylinders - 1;
		else
			limit = cxt->total_sectors - 1;

		if (limit > UINT_MAX)
			limit = UINT_MAX;

		if (l->ext_offset) {
			assert(ext_pe);
			first[l->ext_index] = l->ext_offset;
			last[l->ext_index] = get_abs_partition_end(ext_pe);
		}
	} else {
		assert(ext_pe);
		start = l->ext_offset + cxt->first_lba;
		limit = get_abs_partition_end(ext_pe);
	}
	if (fdisk_context_use_cylinders(cxt))
		for (i = 0; i < cxt->label->nparts_max; i++) {
			first[i] = (fdisk_cround(cxt, first[i]) - 1)
				* fdisk_context_get_units_per_sector(cxt);
		}

	/*
	 * Ask for first sector
	 */
	do {
		sector_t dflt, aligned;

		temp = start;
		dflt = start = get_unused_start(cxt, n, start, first, last);

		/* the default sector should be aligned and unused */
		do {
			aligned = fdisk_align_lba_in_range(cxt, dflt, dflt, limit);
			dflt = get_unused_start(cxt, n, aligned, first, last);
		} while (dflt != aligned && dflt > aligned && dflt < limit);

		if (dflt >= limit)
			dflt = start;
		if (start > limit)
			break;
		if (start >= temp + fdisk_context_get_units_per_sector(cxt)
		    && read) {
			fdisk_info(cxt, _("Sector %llu is already allocated."),
					temp);
			temp = start;
			read = 0;
		}

		if (!read && start == temp) {
			rc = get_start_from_user(cxt, &start, temp, dflt, limit, pa);
			if (rc)
				return rc;
			read = 1;
		}
	} while (start != temp || !read);

	if (n > 4) {			/* NOT for fifth partition */
		struct pte *pe = self_pte(cxt, n);

		pe->offset = start - cxt->first_lba;
		if (pe->offset == l->ext_offset) { /* must be corrected */
			pe->offset++;
			if (cxt->first_lba == 1)
				start++;
		}
	}

	for (i = 0; i < cxt->label->nparts_max; i++) {
		struct pte *pe = self_pte(cxt, i);

		if (start < pe->offset && limit >= pe->offset)
			limit = pe->offset - 1;
		if (start < first[i] && limit >= first[i])
			limit = first[i] - 1;
	}
	if (start > limit) {
		fdisk_info(cxt, _("No free sectors available."));
		if (n > 4)
			cxt->label->nparts_max--;
		return -ENOSPC;
	}

	/*
	 * Ask for last sector
	 */
	if (fdisk_cround(cxt, start) == fdisk_cround(cxt, limit))
		stop = limit;
	else if (pa && pa->end_follow_default)
		stop = limit;
	else if (pa && pa->size) {
		stop = start + pa->size;
		isrel = 1;
	} else {
		/* ask user by dialog */
		struct fdisk_ask *ask = fdisk_new_ask();

		if (!ask)
			return -ENOMEM;
		fdisk_ask_set_type(ask, FDISK_ASKTYPE_OFFSET);

		if (fdisk_context_use_cylinders(cxt)) {
			fdisk_ask_set_query(ask, _("Last cylinder, +cylinders or +size{K,M,G,T,P}"));
			fdisk_ask_number_set_unit(ask,
				     cxt->sector_size *
				     fdisk_context_get_units_per_sector(cxt));
		} else {
			fdisk_ask_set_query(ask, _("Last sector, +sectors or +size{K,M,G,T,P}"));
			fdisk_ask_number_set_unit(ask,cxt->sector_size);
		}

		fdisk_ask_number_set_low(ask, fdisk_cround(cxt, start));
		fdisk_ask_number_set_default(ask, fdisk_cround(cxt, limit));
		fdisk_ask_number_set_high(ask, fdisk_cround(cxt, limit));
		fdisk_ask_number_set_base(ask, fdisk_cround(cxt, start));	/* base for relative input */

		rc = fdisk_do_ask(cxt, ask);
		stop = fdisk_ask_number_get_result(ask);
		isrel = fdisk_ask_number_is_relative(ask);
		fdisk_free_ask(ask);
		if (rc)
			return rc;
		if (fdisk_context_use_cylinders(cxt)) {
			stop = stop * fdisk_context_get_units_per_sector(cxt) - 1;
			if (stop >limit)
				stop = limit;
		}
	}

	if (stop > limit)
		stop = limit;

	if (stop < limit) {
		if (isrel && alignment_required(cxt)) {
			/* the last sector has not been exactly requested (but
			 * defined by +size{K,M,G} convention), so be smart and
			 * align the end of the partition. The next partition
			 * will start at phy.block boundary.
			 */
			stop = fdisk_align_lba_in_range(cxt, stop, start, limit) - 1;
			if (stop > limit)
				stop = limit;
		}
	}

	set_partition(cxt, n, 0, start, stop, sys);
	if (n > 4) {
		struct pte *pe = self_pte(cxt, n);
		set_partition(cxt, n - 1, 1, pe->offset, stop,
				MBR_DOS_EXTENDED_PARTITION);
	}

	if (IS_EXTENDED(sys)) {
		struct pte *pe4 = self_pte(cxt, 4);
		struct pte *pen = self_pte(cxt, n);

		l->ext_index = n;
		pen->ex_entry = p;
		pe4->offset = l->ext_offset = start;
		pe4->sectorbuffer = calloc(1, cxt->sector_size);
		if (!pe4->sectorbuffer)
			return -ENOMEM;
		DBG(LABEL, dbgprint("DOS: add partition, sector buffer %p", pe4->sectorbuffer));
		pe4->private_sectorbuffer = 1;
		pe4->pt_entry = mbr_get_partition(pe4->sectorbuffer, 0);
		pe4->ex_entry = pe4->pt_entry + 1;

		partition_set_changed(cxt, 4, 1);
		cxt->label->nparts_max = 5;
	}

	fdisk_label_set_changed(cxt->label, 1);
	return 0;
}

static int add_logical(struct fdisk_context *cxt, struct fdisk_partition *pa)
{
	struct dos_partition *p4 = self_partition(cxt, 4);

	assert(cxt);
	assert(cxt->label);

	if (cxt->label->nparts_max > 5 || !is_cleared_partition(p4)) {
		struct pte *pe = self_pte(cxt, cxt->label->nparts_max);

		pe->sectorbuffer = calloc(1, cxt->sector_size);
		if (!pe->sectorbuffer)
			return -ENOMEM;
		DBG(LABEL, dbgprint("DOS: add logical, sector buffer %p", pe->sectorbuffer));
		pe->private_sectorbuffer = 1;
		pe->pt_entry = mbr_get_partition(pe->sectorbuffer, 0);
		pe->ex_entry = pe->pt_entry + 1;
		pe->offset = 0;

		partition_set_changed(cxt, cxt->label->nparts_max, 1);
		cxt->label->nparts_max++;
	}
	fdisk_info(cxt, _("Adding logical partition %zu"),
			cxt->label->nparts_max);
	return add_partition(cxt, cxt->label->nparts_max - 1, pa);
}

static void check(struct fdisk_context *cxt, size_t n,
	   unsigned int h, unsigned int s, unsigned int c,
	   unsigned int start)
{
	unsigned int total, real_s, real_c;

	real_s = sector(s) - 1;
	real_c = cylinder(s, c);
	total = (real_c * cxt->geom.sectors + real_s) * cxt->geom.heads + h;

	if (!total)
		fdisk_warnx(cxt, _("Partition %zu: contains sector 0"), n);
	if (h >= cxt->geom.heads)
		fdisk_warnx(cxt, _("Partition %zu: head %d greater than "
				   "maximum %d"), n, h + 1, cxt->geom.heads);
	if (real_s >= cxt->geom.sectors)
		fdisk_warnx(cxt, _("Partition %zu: sector %d greater than "
				   "maximum %llu"), n, s, cxt->geom.sectors);
	if (real_c >= cxt->geom.cylinders)
		fdisk_warnx(cxt, _("Partition %zu: cylinder %d greater than "
				   "maximum %llu"),
				n, real_c + 1,
				cxt->geom.cylinders);

	if (cxt->geom.cylinders <= 1024 && start != total)
		fdisk_warnx(cxt, _("Partition %zu: previous sectors %u "
				   "disagrees with total %u"), n, start, total);
}

/* check_consistency() and long2chs() added Sat Mar 6 12:28:16 1993,
 * faith@cs.unc.edu, based on code fragments from pfdisk by Gordon W. Ross,
 * Jan.  1990 (version 1.2.1 by Gordon W. Ross Aug. 1990; Modified by S.
 * Lubkin Oct.  1991). */

static void
long2chs(struct fdisk_context *cxt, unsigned long ls,
	 unsigned int *c, unsigned int *h, unsigned int *s) {
	int spc = cxt->geom.heads * cxt->geom.sectors;

	*c = ls / spc;
	ls = ls % spc;
	*h = ls / cxt->geom.sectors;
	*s = ls % cxt->geom.sectors + 1;	/* sectors count from 1 */
}

static void check_consistency(struct fdisk_context *cxt, struct dos_partition *p,
			      size_t partition)
{
	unsigned int pbc, pbh, pbs;	/* physical beginning c, h, s */
	unsigned int pec, peh, pes;	/* physical ending c, h, s */
	unsigned int lbc, lbh, lbs;	/* logical beginning c, h, s */
	unsigned int lec, leh, les;	/* logical ending c, h, s */

	if (!is_dos_compatible(cxt))
		return;

	if (!cxt->geom.heads || !cxt->geom.sectors || (partition >= 4))
		return;		/* do not check extended partitions */

	/* physical beginning c, h, s */
	pbc = (p->bc & 0xff) | ((p->bs << 2) & 0x300);
	pbh = p->bh;
	pbs = p->bs & 0x3f;

	/* physical ending c, h, s */
	pec = (p->ec & 0xff) | ((p->es << 2) & 0x300);
	peh = p->eh;
	pes = p->es & 0x3f;

	/* compute logical beginning (c, h, s) */
	long2chs(cxt, dos_partition_get_start(p), &lbc, &lbh, &lbs);

	/* compute logical ending (c, h, s) */
	long2chs(cxt, dos_partition_get_start(p) + dos_partition_get_size(p) - 1, &lec, &leh, &les);

	/* Same physical / logical beginning? */
	if (cxt->geom.cylinders <= 1024
	    && (pbc != lbc || pbh != lbh || pbs != lbs)) {
		fdisk_warnx(cxt, _("Partition %zu: different physical/logical "
			"beginnings (non-Linux?): "
			"phys=(%d, %d, %d), logical=(%d, %d, %d)"),
			partition + 1,
			pbc, pbh, pbs,
			lbc, lbh, lbs);
	}

	/* Same physical / logical ending? */
	if (cxt->geom.cylinders <= 1024
	    && (pec != lec || peh != leh || pes != les)) {
		fdisk_warnx(cxt, _("Partition %zu: different physical/logical "
			"endings: phys=(%d, %d, %d), logical=(%d, %d, %d)"),
			partition + 1,
			pec, peh, pes,
			lec, leh, les);
	}

	/* Ending on cylinder boundary? */
	if (peh != (cxt->geom.heads - 1) || pes != cxt->geom.sectors) {
		fdisk_warnx(cxt, _("Partition %zu: does not end on "
				   "cylinder boundary."),
			partition + 1);
	}
}

static int dos_verify_disklabel(struct fdisk_context *cxt)
{
	size_t i, j;
	sector_t total = 1, n_sectors = cxt->total_sectors;
	unsigned long long first[cxt->label->nparts_max],
			   last[cxt->label->nparts_max];
	struct dos_partition *p;
	struct fdisk_dos_label *l = self_label(cxt);

	assert(fdisk_is_disklabel(cxt, DOS));

	fill_bounds(cxt, first, last);
	for (i = 0; i < cxt->label->nparts_max; i++) {
		struct pte *pe = self_pte(cxt, i);

		p = self_partition(cxt, i);
		if (is_used_partition(p) && !IS_EXTENDED(p->sys_ind)) {
			check_consistency(cxt, p, i);
			fdisk_warn_alignment(cxt, get_abs_partition_start(pe), i);
			if (get_abs_partition_start(pe) < first[i])
				fdisk_warnx(cxt, _(
					"Partition %zu: bad start-of-data."),
					 i + 1);

			check(cxt, i + 1, p->eh, p->es, p->ec, last[i]);
			total += last[i] + 1 - first[i];

			for (j = 0; j < i; j++) {
				if ((first[i] >= first[j] && first[i] <= last[j])
				    || ((last[i] <= last[j] && last[i] >= first[j]))) {

					fdisk_warnx(cxt, _("Partition %zu: "
						"overlaps partition %zu."),
						j + 1, i + 1);

					total += first[i] >= first[j] ?
						first[i] : first[j];
					total -= last[i] <= last[j] ?
						last[i] : last[j];
				}
			}
		}
	}

	if (l->ext_offset) {
		sector_t e_last;
		struct pte *ext_pe = self_pte(cxt, l->ext_index);

		e_last = get_abs_partition_end(ext_pe);

		for (i = 4; i < cxt->label->nparts_max; i++) {
			total++;
			p = self_partition(cxt, i);

			if (!p->sys_ind) {
				if (i != 4 || i + 1 < cxt->label->nparts_max)
					fdisk_warnx(cxt,
						_("Partition %zu: empty."),
						i + 1);
			} else if (first[i] < l->ext_offset
				   || last[i] > e_last) {

				fdisk_warnx(cxt, _("Logical partition %zu: "
					"not entirely in partition %zu."),
					i + 1, l->ext_index + 1);
			}
		}
	}

	if (total > n_sectors)
		fdisk_warnx(cxt, _("Total allocated sectors %llu greater "
			"than the maximum %llu."), total, n_sectors);
	else if (total < n_sectors)
		fdisk_warnx(cxt, _("Remaining %lld unallocated %ld-byte "
			"sectors."), n_sectors - total, cxt->sector_size);

	return 0;
}

/*
 * Ask the user for new partition type information (logical, extended).
 * This function calls the actual partition adding logic - add_partition.
 *
 * API callback.
 */
static int dos_add_partition(struct fdisk_context *cxt,
			     struct fdisk_partition *pa)
{
	size_t i, free_primary = 0;
	int rc = 0;
	struct fdisk_dos_label *l;
	struct pte *ext_pe;

	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	l = self_label(cxt);
	ext_pe = l->ext_offset ? self_pte(cxt, l->ext_index) : NULL;

	/* pa specifies start within extended partition, add logical */
	if (pa && pa->start && ext_pe
	    && pa->start >= l->ext_offset
	    && pa->start <= get_abs_partition_end(ext_pe)) {
		rc = add_logical(cxt, pa);
		goto done;

	/* pa specifies start, but outside extended partition */
	} else if (pa && pa->start && l->ext_offset) {
		int j;

		j = get_partition_unused_primary(cxt, pa);
		if (j >= 0) {
			rc = add_partition(cxt, j, pa);
			goto done;
		}

	}

	for (i = 0; i < 4; i++) {
		struct dos_partition *p = self_partition(cxt, i);
		free_primary += !is_used_partition(p);
	}

	if (!free_primary && cxt->label->nparts_max >= MAXIMUM_PARTS) {
		fdisk_info(cxt, _("The maximum number of partitions has "
				  "been created."));
		return -EINVAL;
	}
	rc = 1;

	if (!free_primary) {
		if (l->ext_offset) {
			fdisk_info(cxt, _("All primary partitions are in use."));
			rc = add_logical(cxt, pa);
		} else
			fdisk_info(cxt, _("If you want to create more than "
				"four partitions, you must replace a "
				"primary partition with an extended "
				"partition first."));

	} else if (cxt->label->nparts_max >= MAXIMUM_PARTS) {
		int j;

		fdisk_info(cxt, _("All logical partitions are in use. "
				  "Adding a primary partition."));
		j = get_partition_unused_primary(cxt, pa);
		if (j >= 0)
			rc = add_partition(cxt, j, pa);
	} else {
		char hint[BUFSIZ];
		struct fdisk_ask *ask;
		int c;

		ask = fdisk_new_ask();
		if (!ask)
			return -ENOMEM;
		fdisk_ask_set_type(ask, FDISK_ASKTYPE_MENU);
		fdisk_ask_set_query(ask, _("Partition type"));
		fdisk_ask_menu_set_default(ask, free_primary == 1
						&& !l->ext_offset ? 'e' : 'p');
		snprintf(hint, sizeof(hint),
				_("%zu primary, %d extended, %zu free"),
				4 - (l->ext_offset ? 1 : 0) - free_primary,
				l->ext_offset ? 1 : 0,
				free_primary);

		fdisk_ask_menu_add_item(ask, 'p', _("primary"), hint);
		if (!l->ext_offset)
			fdisk_ask_menu_add_item(ask, 'e', _("extended"), _("container for logical partitions"));
		else
			fdisk_ask_menu_add_item(ask, 'l', _("logical"), _("numbered from 5"));

		rc = fdisk_do_ask(cxt, ask);
		if (rc)
			return rc;
		fdisk_ask_menu_get_result(ask, &c);
		fdisk_free_ask(ask);

		if (c == 'p') {
			int j = get_partition_unused_primary(cxt, pa);
			if (j >= 0)
				rc = add_partition(cxt, j, pa);
			goto done;
		} else if (c == 'l' && l->ext_offset) {
			rc = add_logical(cxt, pa);
			goto done;
		} else if (c == 'e' && !l->ext_offset) {
			int j = get_partition_unused_primary(cxt, pa);
			if (j >= 0) {
				struct fdisk_partition xpa = { .type = NULL };
				struct fdisk_parttype *t;

				t = fdisk_get_parttype_from_code(cxt,
						MBR_DOS_EXTENDED_PARTITION);
				if (!pa)
					pa = &xpa;
				fdisk_partition_set_type(pa, t);
				rc = add_partition(cxt, j, pa);
			}
			goto done;
		} else
			fdisk_warnx(cxt, _("Invalid partition type `%c'."), c);
	}
done:
	if (rc == 0)
		cxt->label->nparts_cur++;
	return rc;
}

static int write_sector(struct fdisk_context *cxt, sector_t secno,
			       unsigned char *buf)
{
	int rc;

	rc = seek_sector(cxt, secno);
	if (rc != 0) {
		fdisk_warn(cxt, _("Cannot write sector %jd: seek failed"),
				(uintmax_t) secno);
		return rc;
	}

	DBG(LABEL, dbgprint("DOS: writting to sector %ju", (uintmax_t) secno));

	if (write(cxt->dev_fd, buf, cxt->sector_size) != (ssize_t) cxt->sector_size)
		return -errno;
	return 0;
}

static int dos_write_disklabel(struct fdisk_context *cxt)
{
	struct fdisk_dos_label *l = self_label(cxt);
	size_t i;
	int rc = 0, mbr_changed = 0;

	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	mbr_changed = l->non_pt_changed;

	/* MBR (primary partitions) */
	if (!mbr_changed) {
		for (i = 0; i < 4; i++) {
			struct pte *pe = self_pte(cxt, i);
			if (pe->changed)
				mbr_changed = 1;
		}
	}
	if (mbr_changed) {
		mbr_set_magic(cxt->firstsector);
		rc = write_sector(cxt, 0, cxt->firstsector);
		if (rc)
			goto done;
	}

	if (cxt->label->nparts_max <= 4 && l->ext_offset) {
		/* we have empty extended partition, check if the partition has
		 * been modified and then cleanup possible remaining EBR  */
		struct pte *pe = self_pte(cxt, l->ext_index);
		unsigned char empty[512] = { 0 };
		sector_t off = pe ? get_abs_partition_start(pe) : 0;

		if (off && pe->changed) {
			mbr_set_magic(empty);
			write_sector(cxt, off, empty);
		}
	}

	/* EBR (logical partitions) */
	for (i = 4; i < cxt->label->nparts_max; i++) {
		struct pte *pe = self_pte(cxt, i);

		if (pe->changed) {
			mbr_set_magic(pe->sectorbuffer);
			rc = write_sector(cxt, pe->offset, pe->sectorbuffer);
			if (rc)
				goto done;
		}
	}

done:
	return rc;
}

static int dos_locate_disklabel(struct fdisk_context *cxt, int n,
		const char **name, off_t *offset, size_t *size)
{
	assert(cxt);

	*name = NULL;
	*offset = 0;
	*size = 0;

	switch (n) {
	case 0:
		*name = "MBR";
		*offset = 0;
		*size = 512;
		break;
	default:
		/* extended partitions */
		if (n - 1 + 4 < cxt->label->nparts_max) {
			struct pte *pe = self_pte(cxt, n - 1 + 4);

			assert(pe->private_sectorbuffer);

			*name = "EBR";
			*offset = pe->offset * cxt->sector_size;
			*size = 512;
		} else
			return 1;
		break;
	}

	return 0;
}

static int dos_set_parttype(
		struct fdisk_context *cxt,
		size_t partnum,
		struct fdisk_parttype *t)
{
	struct dos_partition *p;

	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	if (partnum >= cxt->label->nparts_max || !t || t->type > UINT8_MAX)
		return -EINVAL;

	p = self_partition(cxt, partnum);
	if (t->type == p->sys_ind)
		return 0;

	if (IS_EXTENDED(p->sys_ind) || IS_EXTENDED(t->type)) {
		fdisk_warnx(cxt, _("You cannot change a partition into an "
			"extended one or vice versa. Delete it first."));
		return -EINVAL;
	}

	if (is_dos_partition(t->type) || is_dos_partition(p->sys_ind))
	    fdisk_info(cxt, _("If you have created or modified any DOS 6.x "
		"partitions, please see the fdisk documentation for additional "
		"information."));

	if (!t->type)
		fdisk_warnx(cxt, _("Type 0 means free space to many systems. "
				   "Having partitions of type 0 is probably unwise."));
	p->sys_ind = t->type;

	partition_set_changed(cxt, partnum, 1);
	return 0;
}

/*
 * Check whether partition entries are ordered by their starting positions.
 * Return 0 if OK. Return i if partition i should have been earlier.
 * Two separate checks: primary and logical partitions.
 */
static int wrong_p_order(struct fdisk_context *cxt, size_t *prev)
{
	size_t last_p_start_pos = 0, p_start_pos;
	size_t i, last_i = 0;

	for (i = 0 ; i < cxt->label->nparts_max; i++) {

		struct pte *pe = self_pte(cxt, i);
		struct dos_partition *p = pe->pt_entry;

		if (i == 4) {
			last_i = 4;
			last_p_start_pos = 0;
		}
		if (is_used_partition(p)) {
			p_start_pos = get_abs_partition_start(pe);

			if (last_p_start_pos > p_start_pos) {
				if (prev)
					*prev = last_i;
				return i;
			}

			last_p_start_pos = p_start_pos;
			last_i = i;
		}
	}
	return 0;
}

static int dos_list_disklabel(struct fdisk_context *cxt)
{
	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	return 0;
}

static int dos_get_partition(struct fdisk_context *cxt, size_t n,
			     struct fdisk_partition *pa)
{
	struct dos_partition *p;
	struct pte *pe;
	struct fdisk_dos_label *lb;

	assert(cxt);
	assert(pa);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	lb = self_label(cxt);
	pe = self_pte(cxt, n);
	p = pe->pt_entry;
	pa->used = !is_cleared_partition(p);
	if (!pa->used)
		return 0;

	pa->type = dos_partition_parttype(cxt, p);
	pa->boot = p->boot_ind ? p->boot_ind == ACTIVE_FLAG ? '*' : '?' : ' ';
	pa->start = get_abs_partition_start(pe);
	pa->end = get_abs_partition_end(pe);
	pa->size = dos_partition_get_size(p);
	pa->container = lb->ext_offset && n == lb->ext_index;

	if (n >= 4)
		pa->parent_partno = lb->ext_index;

	if (asprintf(&pa->attrs, "%02x", p->boot_ind) < 0)
		return -ENOMEM;

	/* start C/H/S */
	if (asprintf(&pa->start_addr, "%d/%d/%d",
				cylinder(p->bs, p->bc),
				sector(p->bs),
				p->bh) < 0)
		return -ENOMEM;

	/* end C/H/S */
	if (asprintf(&pa->end_addr, "%d/%d/%d",
				cylinder(p->es, p->ec),
				sector(p->es),
				p->eh) < 0)
		return -ENOMEM;

	return 0;
}

/*
 * Fix the chain of logicals.
 * ext_offset is unchanged, the set of sectors used is unchanged
 * The chain is sorted so that sectors increase, and so that
 * starting sectors increase.
 *
 * After this it may still be that cfdisk doesn't like the table.
 * (This is because cfdisk considers expanded parts, from link to
 * end of partition, and these may still overlap.)
 * Now
 *   sfdisk /dev/hda > ohda; sfdisk /dev/hda < ohda
 * may help.
 */
static void fix_chain_of_logicals(struct fdisk_context *cxt)
{
	struct fdisk_dos_label *l = self_label(cxt);
	size_t j, oj, ojj, sj, sjj;
	struct dos_partition *pj,*pjj,tmp;

	/* Stage 1: sort sectors but leave sector of part 4 */
	/* (Its sector is the global ext_offset.) */
stage1:
	for (j = 5; j < cxt->label->nparts_max - 1; j++) {
		oj = l->ptes[j].offset;
		ojj = l->ptes[j + 1].offset;
		if (oj > ojj) {
			l->ptes[j].offset = ojj;
			l->ptes[j + 1].offset = oj;
			pj = l->ptes[j].pt_entry;
			dos_partition_set_start(pj, dos_partition_get_start(pj)+oj-ojj);
			pjj = l->ptes[j + 1].pt_entry;
			dos_partition_set_start(pjj, dos_partition_get_start(pjj)+ojj-oj);
			dos_partition_set_start(l->ptes[j - 1].ex_entry,
				       ojj - l->ext_offset);
			dos_partition_set_start(l->ptes[j].ex_entry,
				       oj - l->ext_offset);
			goto stage1;
		}
	}

	/* Stage 2: sort starting sectors */
stage2:
	for (j = 4; j < cxt->label->nparts_max - 1; j++) {
		pj = l->ptes[j].pt_entry;
		pjj = l->ptes[j + 1].pt_entry;
		sj = dos_partition_get_start(pj);
		sjj = dos_partition_get_start(pjj);
		oj = l->ptes[j].offset;
		ojj = l->ptes[j+1].offset;
		if (oj+sj > ojj+sjj) {
			tmp = *pj;
			*pj = *pjj;
			*pjj = tmp;
			dos_partition_set_start(pj, ojj+sjj-oj);
			dos_partition_set_start(pjj, oj+sj-ojj);
			goto stage2;
		}
	}

	/* Probably something was changed */
	for (j = 4; j < cxt->label->nparts_max; j++)
		l->ptes[j].changed = 1;
}

int fdisk_dos_fix_order(struct fdisk_context *cxt)
{
	struct pte *pei, *pek;
	size_t i,k;

	if (!wrong_p_order(cxt, NULL)) {
		fdisk_info(cxt, _("Nothing to do. Ordering is correct already."));
		return 0;
	}

	while ((i = wrong_p_order(cxt, &k)) != 0 && i < 4) {
		/* partition i should have come earlier, move it */
		/* We have to move data in the MBR */
		struct dos_partition *pi, *pk, *pe, pbuf;
		pei = self_pte(cxt, i);
		pek = self_pte(cxt, k);

		pe = pei->ex_entry;
		pei->ex_entry = pek->ex_entry;
		pek->ex_entry = pe;

		pi = pei->pt_entry;
		pk = pek->pt_entry;

		memmove(&pbuf, pi, sizeof(struct dos_partition));
		memmove(pi, pk, sizeof(struct dos_partition));
		memmove(pk, &pbuf, sizeof(struct dos_partition));

		partition_set_changed(cxt, i, 1);
		partition_set_changed(cxt, k, 1);
	}

	if (i)
		fix_chain_of_logicals(cxt);

	fdisk_info(cxt, _("Done."));
	return 0;
}

int fdisk_dos_move_begin(struct fdisk_context *cxt, size_t i)
{
	struct pte *pe;
	struct dos_partition *p;
	unsigned int new, free_start, curr_start, last;
	uintmax_t res = 0;
	size_t x;
	int rc;

	assert(cxt);
	assert(fdisk_is_disklabel(cxt, DOS));

	pe = self_pte(cxt, i);
	p = pe->pt_entry;

	if (!is_used_partition(p) || IS_EXTENDED (p->sys_ind)) {
		fdisk_warnx(cxt, _("Partition %zu: no data area."), i + 1);
		return 0;
	}

	/* the default start is at the second sector of the disk or at the
	 * second sector of the extended partition
	 */
	free_start = pe->offset ? pe->offset + 1 : 1;

	curr_start = get_abs_partition_start(pe);

	/* look for a free space before the current start of the partition */
	for (x = 0; x < cxt->label->nparts_max; x++) {
		unsigned int end;
		struct pte *prev_pe = self_pte(cxt, x);
		struct dos_partition *prev_p = prev_pe->pt_entry;

		if (!prev_p)
			continue;
		end = get_abs_partition_start(prev_pe)
		      + dos_partition_get_size(prev_p);

		if (is_used_partition(prev_p) &&
		    end > free_start && end <= curr_start)
			free_start = end;
	}

	last = get_abs_partition_end(pe);

	rc = fdisk_ask_number(cxt, free_start, curr_start, last,
			_("New beginning of data"), &res);
	if (rc)
		return rc;

	new = res - pe->offset;

	if (new != dos_partition_get_size(p)) {
		unsigned int sects = dos_partition_get_size(p)
				+ dos_partition_get_start(p) - new;

		dos_partition_set_size(p, sects);
		dos_partition_set_start(p, new);

		partition_set_changed(cxt, i, 1);
	}

	return rc;
}

static int dos_partition_is_used(
		struct fdisk_context *cxt,
		size_t i)
{
	struct dos_partition *p;

	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	if (i >= cxt->label->nparts_max)
		return 0;

	p = self_partition(cxt, i);

	return p && !is_cleared_partition(p);
}

static int dos_toggle_partition_flag(
		struct fdisk_context *cxt,
		size_t i,
		unsigned long flag)
{
	struct dos_partition *p;

	assert(cxt);
	assert(cxt->label);
	assert(fdisk_is_disklabel(cxt, DOS));

	if (i >= cxt->label->nparts_max)
		return -EINVAL;

	p = self_partition(cxt, i);

	switch (flag) {
	case DOS_FLAG_ACTIVE:
		if (IS_EXTENDED(p->sys_ind) && !p->boot_ind)
			fdisk_warnx(cxt, _("Partition %zu: is an extended "
					"partition."), i + 1);

		p->boot_ind = (p->boot_ind ? 0 : ACTIVE_FLAG);
		partition_set_changed(cxt, i, 1);
		fdisk_sinfo(cxt, FDISK_INFO_SUCCESS,
			p->boot_ind ?
			_("The bootable flag on partition %zu is enabled now.") :
			_("The bootable flag on partition %zu is disabled now."),
			i + 1);
		break;
	default:
		return 1;
	}

	return 0;
}

static const struct fdisk_column dos_columns[] =
{
	/* basic */
	{ FDISK_COL_DEVICE,	N_("Device"),	 10,	0 },
	{ FDISK_COL_BOOT,	N_("Boot"),	  1,	0 },
	{ FDISK_COL_START,	N_("Start"),	  5,	TT_FL_RIGHT },
	{ FDISK_COL_END,	N_("End"),	  5,	TT_FL_RIGHT },
	{ FDISK_COL_SECTORS,	N_("Sectors"),	  5,	TT_FL_RIGHT },
	{ FDISK_COL_CYLINDERS,	N_("Cylinders"),  5,	TT_FL_RIGHT },
	{ FDISK_COL_SIZE,	N_("Size"),	  5,	TT_FL_RIGHT, FDISK_COLFL_EYECANDY },
	{ FDISK_COL_TYPEID,	N_("Id"),	  2,	TT_FL_RIGHT },
	{ FDISK_COL_TYPE,	N_("Type"),	0.1,	TT_FL_TRUNC },

	/* expert mode */
	{ FDISK_COL_SADDR,	N_("Start-C/H/S"), 1,   TT_FL_RIGHT, FDISK_COLFL_DETAIL },
	{ FDISK_COL_EADDR,	N_("End-C/H/S"),   1,   TT_FL_RIGHT, FDISK_COLFL_DETAIL },
	{ FDISK_COL_ATTR,	N_("Attrs"),	   2,   TT_FL_RIGHT, FDISK_COLFL_DETAIL }

};

static const struct fdisk_label_operations dos_operations =
{
	.probe		= dos_probe_label,
	.write		= dos_write_disklabel,
	.verify		= dos_verify_disklabel,
	.create		= dos_create_disklabel,
	.locate		= dos_locate_disklabel,
	.list		= dos_list_disklabel,
	.get_id		= dos_get_disklabel_id,
	.set_id		= dos_set_disklabel_id,

	.get_part	= dos_get_partition,
	.add_part	= dos_add_partition,

	.part_delete	= dos_delete_partition,
	.part_set_type	= dos_set_parttype,

	.part_toggle_flag = dos_toggle_partition_flag,
	.part_is_used	= dos_partition_is_used,

	.reset_alignment = dos_reset_alignment,

	.deinit		= dos_deinit,
};

/*
 * allocates DOS in-memory stuff
 */
struct fdisk_label *fdisk_new_dos_label(struct fdisk_context *cxt)
{
	struct fdisk_label *lb;
	struct fdisk_dos_label *dos;

	assert(cxt);

	dos = calloc(1, sizeof(*dos));
	if (!dos)
		return NULL;

	/* initialize generic part of the driver */
	lb = (struct fdisk_label *) dos;
	lb->name = "dos";
	lb->id = FDISK_DISKLABEL_DOS;
	lb->op = &dos_operations;
	lb->parttypes = dos_parttypes;
	lb->nparttypes = ARRAY_SIZE(dos_parttypes);
	lb->columns = dos_columns;
	lb->ncolumns = ARRAY_SIZE(dos_columns);

	return lb;
}

/*
 * Public label specific functions
 */

int fdisk_dos_enable_compatible(struct fdisk_label *lb, int enable)
{
	struct fdisk_dos_label *dos = (struct fdisk_dos_label *) lb;

	if (!lb)
		return -EINVAL;

	dos->compatible = enable;
	if (enable)
		lb->flags |= FDISK_LABEL_FL_REQUIRE_GEOMETRY;
	return 0;
}

int fdisk_dos_is_compatible(struct fdisk_label *lb)
{
	return ((struct fdisk_dos_label *) lb)->compatible;
}
