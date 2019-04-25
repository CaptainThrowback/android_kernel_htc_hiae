/*
 * fs/sdcardfs/derived_perm.c
 *
 * Copyright (c) 2013 Samsung Electronics Co. Ltd
 *   Authors: Daeho Jeong, Woojoong Lee, Seunghwan Hyun,
 *               Sunghwan Yun, Sungjong Seo
 *
 * This program has been developed as a stackable file system based on
 * the WrapFS which written by
 *
 * Copyright (c) 1998-2011 Erez Zadok
 * Copyright (c) 2009     Shrikar Archak
 * Copyright (c) 2003-2011 Stony Brook University
 * Copyright (c) 2003-2011 The Research Foundation of SUNY
 *
 * This file is dual licensed.  It may be redistributed and/or modified
 * under the terms of the Apache 2.0 License OR version 2 of the GNU
 * General Public License.
 */

#include "sdcardfs.h"

static void inherit_derived_state(struct inode *parent, struct inode *child)
{
	struct sdcardfs_inode_info *pi = SDCARDFS_I(parent);
	struct sdcardfs_inode_info *ci = SDCARDFS_I(child);

	ci->perm = PERM_INHERIT;
	ci->userid = pi->userid;
	ci->d_uid = pi->d_uid;
	ci->under_android = pi->under_android;
	ci->top = pi->top;
}

void setup_derived_state(struct inode *inode, perm_t perm, userid_t userid,
                        uid_t uid, bool under_android, struct inode *top)
{
	struct sdcardfs_inode_info *info = SDCARDFS_I(inode);

	info->perm = perm;
	info->userid = userid;
	info->d_uid = uid;
	info->under_android = under_android;
	info->top = top;
}

void get_derived_permission_new(struct dentry *parent, struct dentry *dentry, struct dentry *newdentry)
{
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dentry->d_sb);
	struct sdcardfs_inode_info *info = SDCARDFS_I(dentry->d_inode);
	struct sdcardfs_inode_info *parent_info= SDCARDFS_I(parent->d_inode);
	appid_t appid;


	inherit_derived_state(parent->d_inode, dentry->d_inode);

	
	switch (parent_info->perm) {
		case PERM_INHERIT:
			
			break;
		case PERM_PRE_ROOT:
			
			info->perm = PERM_ROOT;
			info->userid = simple_strtoul(newdentry->d_name.name, NULL, 10);
			info->top = &info->vfs_inode;
			break;
		case PERM_ROOT:
			
			if (!strcasecmp(newdentry->d_name.name, "Android")) {
				
				info->perm = PERM_ANDROID;
				info->under_android = true;
				info->top = &info->vfs_inode;
			}
			break;
		case PERM_ANDROID:
			if (!strcasecmp(newdentry->d_name.name, "data")) {
				
				info->perm = PERM_ANDROID_DATA;
				info->top = &info->vfs_inode;
			} else if (!strcasecmp(newdentry->d_name.name, "obb")) {
				
				info->perm = PERM_ANDROID_OBB;
				info->top = &info->vfs_inode;
				
			} else if (!strcasecmp(newdentry->d_name.name, "media")) {
				
				info->perm = PERM_ANDROID_MEDIA;
				info->top = &info->vfs_inode;
			}
			break;
		case PERM_ANDROID_DATA:
		case PERM_ANDROID_OBB:
		case PERM_ANDROID_MEDIA:
			appid = get_appid(sbi->pkgl_id, newdentry->d_name.name);
			if (appid != 0) {
				info->d_uid = multiuser_get_uid(parent_info->userid, appid);
			}
			info->top = &info->vfs_inode;
			break;
	}
}

void get_derived_permission(struct dentry *parent, struct dentry *dentry)
{
	get_derived_permission_new(parent, dentry, dentry);
}

static int descendant_may_need_fixup(perm_t perm) {
	if (perm == PERM_PRE_ROOT || perm == PERM_ROOT || perm == PERM_ANDROID)
		return 1;
	return 0;
}

static int needs_fixup(perm_t perm) {
	if (perm == PERM_ANDROID_DATA || perm == PERM_ANDROID_OBB
			|| perm == PERM_ANDROID_MEDIA)
		return 1;
	return 0;
}

void fixup_perms_recursive(struct dentry *dentry, const char* name, size_t len) {
	struct dentry *child;
	struct sdcardfs_inode_info *info;
	if (!dentry || !dentry->d_inode)
		return;
	info = SDCARDFS_I(dentry->d_inode);

	if (needs_fixup(info->perm)) {
		mutex_lock(&dentry->d_inode->i_mutex);
		child = lookup_one_len(name, dentry, len);
		if (dentry && dentry->d_inode)
			mutex_unlock(&dentry->d_inode->i_mutex);
		else {
			printk(KERN_ERR "Storage Error Bugcheck: dentry or dentry->d_inode is NULL!\n");
			dput(child);
			return;
		}
		if (!IS_ERR(child)) {
			if (child->d_inode) {
				get_derived_permission(dentry, child);
				fix_derived_permission(child->d_inode);
			}
			dput(child);
		}
	} else 	if (descendant_may_need_fixup(info->perm)) {
		mutex_lock(&dentry->d_inode->i_mutex);
		list_for_each_entry(child, &dentry->d_subdirs, d_child) {
				dget(child);
				fixup_perms_recursive(child, name, len);
				dput(child);
		}
		mutex_unlock(&dentry->d_inode->i_mutex);
	}
}

void fixup_top_recursive(struct dentry *parent) {
	struct dentry *dentry;
	struct sdcardfs_inode_info *info;
	if (!parent->d_inode)
		return;
	info = SDCARDFS_I(parent->d_inode);
	spin_lock(&parent->d_lock);
	list_for_each_entry(dentry, &parent->d_subdirs, d_child) {
		if (dentry->d_inode) {
			if (SDCARDFS_I(parent->d_inode)->top != SDCARDFS_I(dentry->d_inode)->top) {
				get_derived_permission(parent, dentry);
				fix_derived_permission(dentry->d_inode);
				fixup_top_recursive(dentry);
			}
		}
	}
	spin_unlock(&parent->d_lock);
}

inline void update_derived_permission_lock(struct dentry *dentry)
{
	struct dentry *parent;

	if(!dentry || !dentry->d_inode) {
		printk(KERN_ERR "sdcardfs: %s: invalid dentry\n", __func__);
		return;
	}
	if(IS_ROOT(dentry)) {
		
	} else {
		parent = dget_parent(dentry);
		if(parent) {
			get_derived_permission(parent, dentry);
			dput(parent);
		}
	}
	fix_derived_permission(dentry->d_inode);
}

int need_graft_path(struct dentry *dentry)
{
	int ret = 0;
	struct dentry *parent = dget_parent(dentry);
	struct sdcardfs_inode_info *parent_info= SDCARDFS_I(parent->d_inode);
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dentry->d_sb);

	if(parent_info->perm == PERM_ANDROID &&
			!strcasecmp(dentry->d_name.name, "obb")) {

		
		if(!(sbi->options.multiuser == false
				&& parent_info->userid == 0)) {
			ret = 1;
		}
	}
	dput(parent);
	return ret;
}

int is_obbpath_invalid(struct dentry *dent)
{
	int ret = 0;
	struct sdcardfs_dentry_info *di = SDCARDFS_D(dent);
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dent->d_sb);
	char *path_buf, *obbpath_s;

	spin_lock(&di->lock);
	if(di->orig_path.dentry) {
 		if(!di->lower_path.dentry) {
			ret = 1;
		} else {
			path_get(&di->lower_path);
			

			path_buf = kmalloc(PATH_MAX, GFP_ATOMIC);
			if(!path_buf) {
				ret = 1;
				printk(KERN_ERR "sdcardfs: fail to allocate path_buf in %s.\n", __func__);
			} else {
				obbpath_s = d_path(&di->lower_path, path_buf, PATH_MAX);
				if (d_unhashed(di->lower_path.dentry) ||
					strcasecmp(sbi->obbpath_s, obbpath_s)) {
					ret = 1;
				}
				kfree(path_buf);
			}

			
			path_put(&di->lower_path);
		}
	}
	spin_unlock(&di->lock);
	return ret;
}

int is_base_obbpath(struct dentry *dentry)
{
	int ret = 0;
	struct dentry *parent = dget_parent(dentry);
	struct sdcardfs_inode_info *parent_info= SDCARDFS_I(parent->d_inode);
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dentry->d_sb);

	spin_lock(&SDCARDFS_D(dentry)->lock);
	if (sbi->options.multiuser) {
		if(parent_info->perm == PERM_PRE_ROOT &&
				!strcasecmp(dentry->d_name.name, "obb")) {
			ret = 1;
		}
	} else  if (parent_info->perm == PERM_ANDROID &&
			!strcasecmp(dentry->d_name.name, "obb")) {
		ret = 1;
	}
	spin_unlock(&SDCARDFS_D(dentry)->lock);
	return ret;
}

int setup_obb_dentry(struct dentry *dentry, struct path *lower_path)
{
	int err = 0;
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dentry->d_sb);
	struct path obbpath;

	sdcardfs_set_orig_path(dentry, lower_path);

	err = kern_path(sbi->obbpath_s,
			LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &obbpath);

	if(!err) {
		
		printk(KERN_INFO "sdcardfs: the sbi->obbpath is found\n");
		pathcpy(lower_path, &obbpath);
	} else {
		printk(KERN_INFO "sdcardfs: the sbi->obbpath is not available\n");
	}
	return err;
}

