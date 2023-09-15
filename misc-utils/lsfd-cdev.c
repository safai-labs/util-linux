/*
 * lsfd-cdev.c - handle associations opening character devices
 *
 * Copyright (C) 2021 Red Hat, Inc. All rights reserved.
 * Written by Masatake YAMATO <yamato@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lsfd.h"

static struct list_head miscdevs;

struct miscdev {
	struct list_head miscdevs;
	unsigned long minor;
	char *name;
};

struct cdev {
	struct file file;
	const char *devdrv;
	const struct cdev_ops *cdev_ops;
	void *cdev_data;
};

struct cdev_ops {
	const struct cdev_ops *parent;
	bool (*probe)(const struct cdev *);
	char * (*get_name)(struct cdev *);
	bool (*fill_column)(struct proc *,
			    struct cdev *,
			    struct libscols_line *,
			    int,
			    size_t,
			    char **);
	void (*init)(const struct cdev *);
	void (*free)(const struct cdev *);
	int (*handle_fdinfo)(struct cdev *, const char *, const char *);
};

static bool cdev_fill_column(struct proc *proc __attribute__((__unused__)),
			     struct file *file,
			     struct libscols_line *ln,
			     int column_id,
			     size_t column_index)
{
	struct cdev *cdev = (struct cdev *)file;
	const struct cdev_ops *ops = cdev->cdev_ops;
	char *str = NULL;

	switch(column_id) {
	case COL_NAME:
		if (cdev->cdev_ops->get_name) {
			str = cdev->cdev_ops->get_name(cdev);
			if (str)
				break;
		}
		return false;
	case COL_TYPE:
		if (scols_line_set_data(ln, column_index, "CHR"))
			err(EXIT_FAILURE, _("failed to add output data"));
		return true;
	case COL_DEVTYPE:
		if (scols_line_set_data(ln, column_index,
					"char"))
			err(EXIT_FAILURE, _("failed to add output data"));
		return true;
	case COL_CHRDRV:
		if (cdev->devdrv)
			str = xstrdup(cdev->devdrv);
		else
			xasprintf(&str, "%u",
				  major(file->stat.st_rdev));
		break;
	default:
		while (ops) {
			if (ops->fill_column
			    && ops->fill_column(proc, cdev, ln,
						column_id, column_index, &str))
				goto out;
			ops = ops->parent;
		}
		return false;
	}

 out:
	if (!str)
		err(EXIT_FAILURE, _("failed to add output data"));
	if (scols_line_refer_data(ln, column_index, str))
		err(EXIT_FAILURE, _("failed to add output data"));
	return true;
}

static struct miscdev *new_miscdev(unsigned long minor, const char *name)
{
	struct miscdev *miscdev = xcalloc(1, sizeof(*miscdev));

	INIT_LIST_HEAD(&miscdev->miscdevs);

	miscdev->minor = minor;
	miscdev->name = xstrdup(name);

	return miscdev;
}

static void free_miscdev(struct miscdev *miscdev)
{
	free(miscdev->name);
	free(miscdev);
}

static void read_misc(struct list_head *miscdevs_list, FILE *misc_fp)
{
	unsigned long minor;
	char line[256];
	char name[sizeof(line)];

	while (fgets(line, sizeof(line), misc_fp)) {
		struct miscdev *miscdev;

		if (sscanf(line, "%lu %s", &minor, name) != 2)
			continue;

		miscdev = new_miscdev(minor, name);
		list_add_tail(&miscdev->miscdevs, miscdevs_list);
	}
}

static void cdev_class_initialize(void)
{
	FILE *misc_fp;

	INIT_LIST_HEAD(&miscdevs);

	misc_fp = fopen("/proc/misc", "r");
	if (misc_fp) {
		read_misc(&miscdevs, misc_fp);
		fclose(misc_fp);
	}
}

static void cdev_class_finalize(void)
{
	list_free(&miscdevs, struct miscdev, miscdevs, free_miscdev);
}

const char *get_miscdev(unsigned long minor)
{
	struct list_head *c;
	list_for_each(c, &miscdevs) {
		struct miscdev *miscdev = list_entry(c, struct miscdev, miscdevs);
		if (miscdev->minor == minor)
			return miscdev->name;
	}
	return NULL;
}

/*
 * generic (fallback implementation)
 */
static bool cdev_generic_probe(const struct cdev *cdev __attribute__((__unused__))) {
	return true;
}

static bool cdev_generic_fill_column(struct proc *proc  __attribute__((__unused__)),
				     struct cdev *cdev,
				  struct libscols_line *ln __attribute__((__unused__)),
				  int column_id,
				  size_t column_index __attribute__((__unused__)),
				  char **str)
{
	struct file *file = &cdev->file;

	switch(column_id) {
	case COL_SOURCE:
		if (cdev->devdrv) {
			xasprintf(str, "%s:%u", cdev->devdrv,
				  minor(file->stat.st_rdev));
			return true;
		}
		/* FALL THROUGH */
	case COL_MAJMIN:
		xasprintf(str, "%u:%u",
			  major(file->stat.st_rdev),
			  minor(file->stat.st_rdev));
		return true;
	default:
		return false;
	}
}

static struct cdev_ops cdev_generic_ops = {
	.probe = cdev_generic_probe,
	.fill_column = cdev_generic_fill_column,
};

/*
 * misc device driver
 */
static bool cdev_misc_probe(const struct cdev *cdev) {
	return cdev->devdrv && strcmp(cdev->devdrv, "misc") == 0;
}

static bool cdev_misc_fill_column(struct proc *proc  __attribute__((__unused__)),
				  struct cdev *cdev,
				  struct libscols_line *ln __attribute__((__unused__)),
				  int column_id,
				  size_t column_index __attribute__((__unused__)),
				  char **str)
{
	struct file *file = &cdev->file;
	const char *miscdev;

	switch(column_id) {
	case COL_MISCDEV:
		miscdev = get_miscdev(minor(file->stat.st_rdev));
		if (miscdev)
			*str = xstrdup(miscdev);
		else
			xasprintf(str, "%u",
				  minor(file->stat.st_rdev));
		return true;
	case COL_SOURCE:
		miscdev = get_miscdev(minor(file->stat.st_rdev));
		if (miscdev)
			xasprintf(str, "misc:%s", miscdev);
		else
			xasprintf(str, "misc:%u",
				  minor(file->stat.st_rdev));
		return true;
	}
	return false;
}

static struct cdev_ops cdev_misc_ops = {
	.parent = &cdev_generic_ops,
	.probe = cdev_misc_probe,
	.fill_column = cdev_misc_fill_column,
};

/*
 * tun devcie driver
 */
static bool cdev_tun_probe(const struct cdev *cdev)
{
	const char *miscdev;

	if ((!cdev->devdrv) || strcmp(cdev->devdrv, "misc"))
		return false;

	miscdev = get_miscdev(minor(cdev->file.stat.st_rdev));
	if (miscdev && strcmp(miscdev, "tun") == 0)
		return true;
	return false;
}

static void cdev_tun_free(const struct cdev *cdev)
{
	if (cdev->cdev_data)
		free(cdev->cdev_data);
}

static char * cdev_tun_get_name(struct cdev *cdev)
{
	char *str = NULL;

	if (cdev->cdev_data == NULL)
		return NULL;

	xasprintf(&str, "iface=%s", (const char *)cdev->cdev_data);
	return str;
}

static bool cdev_tun_fill_column(struct proc *proc  __attribute__((__unused__)),
				 struct cdev *cdev,
				 struct libscols_line *ln __attribute__((__unused__)),
				 int column_id,
				 size_t column_index __attribute__((__unused__)),
				 char **str)
{
	switch(column_id) {
	case COL_MISCDEV:
		*str = xstrdup("tun");
		return true;
	case COL_SOURCE:
		*str = xstrdup("misc:tun");
		return true;
	case COL_TUN_IFACE:
		if (cdev->cdev_data) {
			*str = xstrdup(cdev->cdev_data);
			return true;
		}
	}
	return false;
}

static int cdev_tun_handle_fdinfo(struct cdev *cdev, const char *key, const char *val)
{
	if (strcmp(key, "iff") == 0 && cdev->cdev_data == NULL) {
		cdev->cdev_data = xstrdup(val);
		return 1;
	}
	return false;
}

static struct cdev_ops cdev_tun_ops = {
	.parent = &cdev_misc_ops,
	.probe = cdev_tun_probe,
	.free  = cdev_tun_free,
	.get_name = cdev_tun_get_name,
	.fill_column = cdev_tun_fill_column,
	.handle_fdinfo = cdev_tun_handle_fdinfo,
};

static const struct cdev_ops *cdev_ops[] = {
	&cdev_tun_ops,
	&cdev_misc_ops,
	&cdev_generic_ops		  /* This must be at the end. */
};

static const struct cdev_ops *cdev_probe(const struct cdev *cdev)
{
	const struct cdev_ops *r = NULL;

	for (size_t i = 0; i < ARRAY_SIZE(cdev_ops); i++) {
		if (cdev_ops[i]->probe(cdev)) {
			r = cdev_ops[i];
			break;
		}
	}

	assert(r);
	return r;
}

static void init_cdev_content(struct file *file)
{
	struct cdev *cdev = (struct cdev *)file;

	cdev->devdrv = get_chrdrv(major(file->stat.st_rdev));

	cdev->cdev_data = NULL;
	cdev->cdev_ops = cdev_probe(cdev);
	if (cdev->cdev_ops->init)
		cdev->cdev_ops->init(cdev);
}

static void free_cdev_content(struct file *file)
{
	struct cdev *cdev = (struct cdev *)file;

	if (cdev->cdev_ops->free)
		cdev->cdev_ops->free(cdev);
}

static int cdev_handle_fdinfo(struct file *file, const char *key, const char *value)
{
	struct cdev *cdev = (struct cdev *)file;

	if (cdev->cdev_ops->handle_fdinfo)
		return cdev->cdev_ops->handle_fdinfo(cdev, key, value);
	return 0;		/* Should be handled in parents */
}

const struct file_class cdev_class = {
	.super = &file_class,
	.size = sizeof(struct cdev),
	.initialize_class = cdev_class_initialize,
	.finalize_class = cdev_class_finalize,
	.fill_column = cdev_fill_column,
	.initialize_content = init_cdev_content,
	.free_content = free_cdev_content,
	.handle_fdinfo = cdev_handle_fdinfo,
};
