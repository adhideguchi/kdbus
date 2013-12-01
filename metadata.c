/*
 * Copyright (C) 2013 Kay Sievers
 * Copyright (C) 2013 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/slab.h>
#include <linux/file.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/sizes.h>
#include <linux/audit.h>
#include <linux/security.h>

#include "connection.h"
#include "names.h"
#include "metadata.h"

void kdbus_meta_free(struct kdbus_meta *meta)
{
	kfree(meta->data);
}

static struct kdbus_item *
kdbus_meta_append_item(struct kdbus_meta *meta, size_t extra_size)
{
	struct kdbus_item *item;
	size_t size;

	/* get new metadata buffer, pre-allocate at least 512 bytes */
	if (!meta->data) {
		size = roundup_pow_of_two(256 + KDBUS_ALIGN8(extra_size));
		meta->data = kzalloc(size, GFP_KERNEL);
		if (!meta->data)
			return ERR_PTR(-ENOMEM);

		meta->allocated_size = size;
	}

	/* double the pre-allocated buffer size if needed */
	size = meta->size + KDBUS_ALIGN8(extra_size);
	if (size > meta->allocated_size) {
		size_t size_diff;
		struct kdbus_item *data;

		size = roundup_pow_of_two(size);
		size_diff = size - meta->allocated_size;
		data = kmalloc(size, GFP_KERNEL);
		if (!data)
			return ERR_PTR(-ENOMEM);

		memcpy(data, meta->data, meta->size);
		memset((u8 *)data + meta->allocated_size, 0, size_diff);

		kfree(meta->data);
		meta->data = data;
		meta->allocated_size = size;

	}

	/* insert new record */
	item = (struct kdbus_item *)((u8 *)meta->data + meta->size);
	meta->size += KDBUS_ALIGN8(extra_size);

	return item;
}

static int kdbus_meta_append_timestamp(struct kdbus_meta *meta)
{
	struct kdbus_item *item;
	u64 size = KDBUS_ITEM_SIZE(sizeof(struct kdbus_timestamp));
	struct timespec ts;

	item = kdbus_meta_append_item(meta, size);
	if (IS_ERR(item))
		return PTR_ERR(item);

	item->type = KDBUS_ITEM_TIMESTAMP;
	item->size = size;

	ktime_get_ts(&ts);
	item->timestamp.monotonic_ns = timespec_to_ns(&ts);

	ktime_get_real_ts(&ts);
	item->timestamp.realtime_ns = timespec_to_ns(&ts);

	return 0;
}

static int kdbus_meta_append_data(struct kdbus_meta *meta, u64 type,
				  const void *buf, size_t len)
{
	struct kdbus_item *item;
	u64 size;

	if (len == 0)
		return 0;

	size = KDBUS_ITEM_SIZE(len);
	item = kdbus_meta_append_item(meta, size);
	if (IS_ERR(item))
		return PTR_ERR(item);

	item->type = type;
	item->size = KDBUS_PART_HEADER_SIZE + len;
	memcpy(item->data, buf, len);

	return 0;
}

static int kdbus_meta_append_str(struct kdbus_meta *meta, u64 type,
				 const char *str)
{
	return kdbus_meta_append_data(meta, type, str, strlen(str) + 1);
}

static int kdbus_meta_append_src_names(struct kdbus_meta *meta,
				struct kdbus_conn *conn)
{
	struct kdbus_name_entry *name_entry;
	struct kdbus_item *item;
	u64 pos = 0, size, strsize = 0;
	int ret = 0;

	if (!conn)
		return 0;

	mutex_lock(&conn->names_lock);
	list_for_each_entry(name_entry, &conn->names_list, conn_entry)
		strsize += strlen(name_entry->name) + 1;

	/* no names? then don't do anything */
	if (strsize == 0)
		goto exit_unlock;

	size = KDBUS_ITEM_SIZE(strsize);
	item = kdbus_meta_append_item(meta, size);
	if (IS_ERR(item)) {
		ret = PTR_ERR(item);
		goto exit_unlock;
	}

	item->type = KDBUS_ITEM_NAMES;
	item->size = KDBUS_PART_HEADER_SIZE + strsize;

	list_for_each_entry(name_entry, &conn->names_list, conn_entry) {
		strcpy(item->data + pos, name_entry->name);
		pos += strlen(name_entry->name) + 1;
	}

	meta->src_names = item->data;
	meta->src_names_len = pos;

exit_unlock:
	mutex_unlock(&conn->names_lock);

	return ret;
}

static int kdbus_meta_append_cred(struct kdbus_meta *meta)
{
	struct kdbus_creds creds = {};
	struct kdbus_item *item;
	u64 size = KDBUS_ITEM_SIZE(sizeof(struct kdbus_creds));

	creds.uid = from_kuid_munged(current_user_ns(), current_uid());
	creds.gid = from_kgid_munged(current_user_ns(), current_gid());
	creds.pid = current->pid;
	creds.tid = current->tgid;
	creds.starttime = timespec_to_ns(&current->start_time);

	item = kdbus_meta_append_item(meta, size);
	if (IS_ERR(item))
		return PTR_ERR(item);

	item->type = KDBUS_ITEM_CREDS;
	item->size = size;
	memcpy(&item->creds, &creds, sizeof(struct kdbus_creds));

	return 0;
}

static int kdbus_meta_append_exe(struct kdbus_meta *meta)
{
	struct mm_struct *mm = get_task_mm(current);
	struct path *exe_path = NULL;
	int ret = 0;

	if (mm) {
		down_read(&mm->mmap_sem);
		if (mm->exe_file) {
			path_get(&mm->exe_file->f_path);
			exe_path = &mm->exe_file->f_path;
		}
		up_read(&mm->mmap_sem);
		mmput(mm);
	}

	if (exe_path) {
		char *tmp;
		char *pathname;
		size_t len;

		tmp = (char *) __get_free_page(GFP_TEMPORARY | __GFP_ZERO);
		if (!tmp) {
			path_put(exe_path);
			return -ENOMEM;
		}

		pathname = d_path(exe_path, tmp, PAGE_SIZE);
		if (!IS_ERR(pathname)) {
			len = tmp + PAGE_SIZE - pathname;
			ret = kdbus_meta_append_data(meta, KDBUS_ITEM_EXE,
						     pathname, len);
		}

		free_page((unsigned long) tmp);
		path_put(exe_path);
	}

	return ret;
}

static int kdbus_meta_append_cmdline(struct kdbus_meta *meta)
{
	struct mm_struct *mm = current->mm;
	char *tmp;
	int ret = 0;

	tmp = (char *) __get_free_page(GFP_TEMPORARY | __GFP_ZERO);
	if (!tmp)
		return -ENOMEM;

	if (mm && mm->arg_end) {
		size_t len = mm->arg_end - mm->arg_start;

		if (len > PAGE_SIZE)
			len = PAGE_SIZE;

		ret = copy_from_user(tmp, (const char __user *) mm->arg_start, len);
		if (ret == 0)
			ret = kdbus_meta_append_data(meta, KDBUS_ITEM_CMDLINE,
						     tmp, len);
	}

	free_page((unsigned long) tmp);
	return ret;
}

static int kdbus_meta_append_caps(struct kdbus_meta *meta)
{
	const struct cred *cred;
	struct caps {
		u32 cap[_KERNEL_CAPABILITY_U32S];
	} cap[4];
	unsigned int i;

	rcu_read_lock();
	cred = __task_cred(current);
	for (i = 0; i < _KERNEL_CAPABILITY_U32S; i++) {
		cap[0].cap[i] = cred->cap_inheritable.cap[i];
		cap[1].cap[i] = cred->cap_permitted.cap[i];
		cap[2].cap[i] = cred->cap_effective.cap[i];
		cap[3].cap[i] = cred->cap_bset.cap[i];
	}
	rcu_read_unlock();

	/* clear unused bits */
	for (i = 0; i < 4; i++)
		cap[i].cap[CAP_TO_INDEX(CAP_LAST_CAP)] &=
			CAP_TO_MASK(CAP_LAST_CAP + 1) - 1;

	return kdbus_meta_append_data(meta, KDBUS_ITEM_CAPS,
				      cap, sizeof(cap));
}

#ifdef CONFIG_CGROUPS
static int kdbus_meta_append_cgroup(struct kdbus_meta *meta)
{
	char *tmp;
	int ret;

	tmp = (char *) __get_free_page(GFP_TEMPORARY | __GFP_ZERO);
	if (!tmp)
		return -ENOMEM;

	ret = task_cgroup_path(current, tmp, PAGE_SIZE);
	if (ret >= 0)
		ret = kdbus_meta_append_str(meta, KDBUS_ITEM_CGROUP, tmp);

	free_page((unsigned long) tmp);

	return ret;
}
#endif

#ifdef CONFIG_AUDITSYSCALL
static int kdbus_meta_append_audit(struct kdbus_meta *meta)
{
	struct kdbus_audit audit;
	const struct cred *cred;
	uid_t uid;

	rcu_read_lock();
	cred = __task_cred(current);
	uid = from_kuid(cred->user_ns, audit_get_loginuid(current));
	rcu_read_unlock();

	audit.loginuid = uid;
	audit.sessionid = audit_get_sessionid(current);

	return kdbus_meta_append_data(meta, KDBUS_ITEM_AUDIT,
				      &audit, sizeof(struct kdbus_audit));
}
#endif

#ifdef CONFIG_SECURITY
static int kdbus_meta_append_seclabel(struct kdbus_meta *meta)
{
	u32 sid;
	char *label;
	u32 len;
	int ret;

	security_task_getsecid(current, &sid);
	ret = security_secid_to_secctx(sid, &label, &len);
	if (ret == -EOPNOTSUPP)
		return 0;
	if (ret < 0)
		return ret;

	if (label && len > 0)
		ret = kdbus_meta_append_data(meta, KDBUS_ITEM_SECLABEL, label, len);
	security_release_secctx(label, len);

	return ret;
}
#endif

int kdbus_meta_append(struct kdbus_meta *meta,
		      struct kdbus_conn *conn,
		      u64 which)
{
	int ret = 0;

	/* kernel-generated messages */
	if (!conn)
		return 0;

	/* all metadata already added */
	if ((which & meta->attached) == which)
		return 0;

	if (which & KDBUS_ATTACH_TIMESTAMP &&
	    !(meta->attached & KDBUS_ATTACH_TIMESTAMP)) {
		ret = kdbus_meta_append_timestamp(meta);
		if (ret < 0)
			goto exit;

		meta->attached |= KDBUS_ATTACH_TIMESTAMP;
	}

	if (which & KDBUS_ATTACH_CREDS &&
	    !(meta->attached & KDBUS_ATTACH_CREDS)) {
		ret = kdbus_meta_append_cred(meta);
		if (ret < 0)
			goto exit;

		meta->attached |= KDBUS_ATTACH_CREDS;
	}

	if (which & KDBUS_ATTACH_NAMES &&
	    !(meta->attached & KDBUS_ATTACH_NAMES)) {
		ret = kdbus_meta_append_src_names(meta, conn);
		if (ret < 0)
			goto exit;

		meta->attached |= KDBUS_ATTACH_NAMES;
	}

	if (which & KDBUS_ATTACH_COMM &&
	    !(meta->attached & KDBUS_ATTACH_COMM)) {
		char comm[TASK_COMM_LEN];

		get_task_comm(comm, current->group_leader);
		ret = kdbus_meta_append_str(meta, KDBUS_ITEM_TID_COMM, comm);
		if (ret < 0)
			goto exit;

		get_task_comm(comm, current);
		ret = kdbus_meta_append_str(meta, KDBUS_ITEM_PID_COMM, comm);
		if (ret < 0)
			goto exit;

		meta->attached |= KDBUS_ATTACH_COMM;
	}

	if (which & KDBUS_ATTACH_EXE &&
	    !(meta->attached & KDBUS_ATTACH_EXE)) {

		ret = kdbus_meta_append_exe(meta);
		if (ret < 0)
			goto exit;

		meta->attached |= KDBUS_ATTACH_EXE;
	}

	if (which & KDBUS_ATTACH_CMDLINE &&
	    !(meta->attached & KDBUS_ATTACH_CMDLINE)) {
		ret = kdbus_meta_append_cmdline(meta);
		if (ret < 0)
			goto exit;

		meta->attached |= KDBUS_ATTACH_CMDLINE;
	}

	/* we always return a 4 elements, the element size is 1/4  */
	if (which & KDBUS_ATTACH_CAPS &&
	    !(meta->attached & KDBUS_ATTACH_CAPS)) {
		ret = kdbus_meta_append_caps(meta);
		if (ret < 0)
			goto exit;

		meta->attached |= KDBUS_ATTACH_CAPS;
	}

#ifdef CONFIG_CGROUPS
	/* attach the path of the one group hierarchy specified for the bus */
	if (which & KDBUS_ATTACH_CGROUP &&
	    !(meta->attached & KDBUS_ATTACH_CGROUP)) {
		ret = kdbus_meta_append_cgroup(meta);
		if (ret < 0)
			goto exit;

		meta->attached |= KDBUS_ATTACH_CGROUP;
	}
#endif

#ifdef CONFIG_AUDITSYSCALL
	if (which & KDBUS_ATTACH_AUDIT &&
	    !(meta->attached & KDBUS_ATTACH_AUDIT)) {
		ret = kdbus_meta_append_audit(meta);
		if (ret < 0)
			goto exit;

		meta->attached |= KDBUS_ATTACH_AUDIT;
	}
#endif

#ifdef CONFIG_SECURITY
	if (which & KDBUS_ATTACH_SECLABEL &&
	    !(meta->attached & KDBUS_ATTACH_SECLABEL)) {
		ret = kdbus_meta_append_seclabel(meta);
		if (ret < 0)
			goto exit;

		meta->attached |= KDBUS_ATTACH_SECLABEL;
	}
#endif

exit:
	return ret;
}