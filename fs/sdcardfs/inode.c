/*
 * fs/sdcardfs/inode.c
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
#include <linux/fs_struct.h>

const struct cred * override_fsids(struct sdcardfs_sb_info* sbi)
{
	struct cred * cred;
	const struct cred * old_cred;

	cred = prepare_creds();
	if (!cred)
		return NULL;

	cred->fsuid = sbi->options.fs_low_uid;
	cred->fsgid = sbi->options.fs_low_gid;

	old_cred = override_creds(cred);

	return old_cred;
}

void revert_fsids(const struct cred * old_cred)
{
	const struct cred * cur_cred;

	cur_cred = current->cred;
	revert_creds(old_cred);
	put_cred(cur_cred);
}

static int sdcardfs_create(struct inode *dir, struct dentry *dentry,
			 umode_t mode, bool want_excl)
{
	int err = 0;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;
	const struct cred *saved_cred = NULL;
	struct fs_struct *saved_fs;
	struct fs_struct *copied_fs;

	if(!check_caller_access_to_name(dir, dentry->d_name.name)) {
		printk(KERN_INFO "%s: need to check the caller's gid in packages.list\n"
						 "  dentry: %s, task:%s\n",
						 __func__, dentry->d_name.name, current->comm);
		err = -EACCES;
		goto out_eacces;
	}

	
	OVERRIDE_CRED(SDCARDFS_SB(dir->i_sb), saved_cred);

	sdcardfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = mnt_want_write(lower_path.mnt);
	if (err)
		goto out_unlock;

	
	mode = (mode & S_IFMT) | 00664;

	
	saved_fs = current->fs;
	copied_fs = copy_fs_struct(current->fs);
	current->fs = copied_fs;
	current->fs->umask = 0;
	err = vfs_create(lower_parent_dentry->d_inode, lower_dentry, mode, want_excl);
	if (err)
		goto out;

	err = sdcardfs_interpose(dentry, dir->i_sb, &lower_path, SDCARDFS_I(dir)->userid);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, sdcardfs_lower_inode(dir));
	fsstack_copy_inode_size(dir, lower_parent_dentry->d_inode);

out:
	mnt_drop_write(lower_path.mnt);
	current->fs = saved_fs;
	free_fs_struct(copied_fs);
out_unlock:
	unlock_dir(lower_parent_dentry);
	sdcardfs_put_lower_path(dentry, &lower_path);
	REVERT_CRED(saved_cred);
out_eacces:
	return err;
}

#if 0
static int sdcardfs_link(struct dentry *old_dentry, struct inode *dir,
		       struct dentry *new_dentry)
{
	struct dentry *lower_old_dentry;
	struct dentry *lower_new_dentry;
	struct dentry *lower_dir_dentry;
	u64 file_size_save;
	int err;
	struct path lower_old_path, lower_new_path;

	OVERRIDE_CRED(SDCARDFS_SB(dir->i_sb));

	file_size_save = i_size_read(old_dentry->d_inode);
	sdcardfs_get_lower_path(old_dentry, &lower_old_path);
	sdcardfs_get_lower_path(new_dentry, &lower_new_path);
	lower_old_dentry = lower_old_path.dentry;
	lower_new_dentry = lower_new_path.dentry;
	lower_dir_dentry = lock_parent(lower_new_dentry);

	err = mnt_want_write(lower_new_path.mnt);
	if (err)
		goto out_unlock;

	err = vfs_link(lower_old_dentry, lower_dir_dentry->d_inode,
		       lower_new_dentry);
	if (err || !lower_new_dentry->d_inode)
		goto out;

	err = sdcardfs_interpose(new_dentry, dir->i_sb, &lower_new_path);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, lower_new_dentry->d_inode);
	fsstack_copy_inode_size(dir, lower_new_dentry->d_inode);
	set_nlink(old_dentry->d_inode,
		  sdcardfs_lower_inode(old_dentry->d_inode)->i_nlink);
	i_size_write(new_dentry->d_inode, file_size_save);
out:
	mnt_drop_write(lower_new_path.mnt);
out_unlock:
	unlock_dir(lower_dir_dentry);
	sdcardfs_put_lower_path(old_dentry, &lower_old_path);
	sdcardfs_put_lower_path(new_dentry, &lower_new_path);
	REVERT_CRED();
	return err;
}
#endif

static int sdcardfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int err;
	struct dentry *lower_dentry;
	struct inode *lower_dir_inode = sdcardfs_lower_inode(dir);
	struct dentry *lower_dir_dentry;
	struct path lower_path;
	const struct cred *saved_cred = NULL;

	if(!check_caller_access_to_name(dir, dentry->d_name.name)) {
		printk(KERN_INFO "%s: need to check the caller's gid in packages.list\n"
						 "  dentry: %s, task:%s\n",
						 __func__, dentry->d_name.name, current->comm);
		err = -EACCES;
		goto out_eacces;
	}

	
	OVERRIDE_CRED(SDCARDFS_SB(dir->i_sb), saved_cred);

	sdcardfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	dget(lower_dentry);
	lower_dir_dentry = lock_parent(lower_dentry);

	err = mnt_want_write(lower_path.mnt);
	if (err)
		goto out_unlock;
	err = vfs_unlink(lower_dir_inode, lower_dentry);

	if (err == -EBUSY && lower_dentry->d_flags & DCACHE_NFSFS_RENAMED)
		err = 0;
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, lower_dir_inode);
	fsstack_copy_inode_size(dir, lower_dir_inode);
	set_nlink(dentry->d_inode,
		  sdcardfs_lower_inode(dentry->d_inode)->i_nlink);
	dentry->d_inode->i_ctime = dir->i_ctime;
	d_drop(dentry); 
out:
	mnt_drop_write(lower_path.mnt);
out_unlock:
	unlock_dir(lower_dir_dentry);
	dput(lower_dentry);
	sdcardfs_put_lower_path(dentry, &lower_path);
	REVERT_CRED(saved_cred);
out_eacces:
	return err;
}

#if 0
static int sdcardfs_symlink(struct inode *dir, struct dentry *dentry,
			  const char *symname)
{
	int err = 0;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;

	OVERRIDE_CRED(SDCARDFS_SB(dir->i_sb));

	sdcardfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = mnt_want_write(lower_path.mnt);
	if (err)
		goto out_unlock;
	err = vfs_symlink(lower_parent_dentry->d_inode, lower_dentry, symname);
	if (err)
		goto out;
	err = sdcardfs_interpose(dentry, dir->i_sb, &lower_path);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, sdcardfs_lower_inode(dir));
	fsstack_copy_inode_size(dir, lower_parent_dentry->d_inode);

out:
	mnt_drop_write(lower_path.mnt);
out_unlock:
	unlock_dir(lower_parent_dentry);
	sdcardfs_put_lower_path(dentry, &lower_path);
	REVERT_CRED();
	return err;
}
#endif

static int touch(char *abs_path, mode_t mode) {
	struct file *filp = filp_open(abs_path, O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW, mode);
	if (IS_ERR(filp)) {
		if (PTR_ERR(filp) == -EEXIST) {
			return 0;
		}
		else {
			printk(KERN_ERR "sdcardfs: failed to open(%s): %ld\n",
						abs_path, PTR_ERR(filp));
			return PTR_ERR(filp);
		}
	}
	filp_close(filp, current->files);
	return 0;
}

static int sdcardfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int err = 0;
	int make_nomedia_in_obb = 0;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dentry->d_sb);
	const struct cred *saved_cred = NULL;
	struct sdcardfs_inode_info *pi = SDCARDFS_I(dir);
	int touch_err = 0;
	struct fs_struct *saved_fs;
	struct fs_struct *copied_fs;

	if(!check_caller_access_to_name(dir, dentry->d_name.name)) {
		printk(KERN_INFO "%s: need to check the caller's gid in packages.list\n"
						 "  dentry: %s, task:%s\n",
						 __func__, dentry->d_name.name, current->comm);
		err = -EACCES;
		goto out_eacces;
	}

	
	OVERRIDE_CRED(SDCARDFS_SB(dir->i_sb), saved_cred);

	
	if (!check_min_free_space(dentry, 0, 1)) {
		printk(KERN_INFO "sdcardfs: No minimum free space.\n");
		err = -ENOSPC;
		goto out_revert;
	}

	
	sdcardfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = mnt_want_write(lower_path.mnt);
	if (err) {
		unlock_dir(lower_parent_dentry);
		goto out_unlock;
	}

	
	mode = (mode & S_IFMT) | 00775;

	
	saved_fs = current->fs;
	copied_fs = copy_fs_struct(current->fs);
	current->fs = copied_fs;
	current->fs->umask = 0;
	err = vfs_mkdir(lower_parent_dentry->d_inode, lower_dentry, mode);

	if (err) {
		unlock_dir(lower_parent_dentry);
		goto out;
	}

	err = sdcardfs_interpose(dentry, dir->i_sb, &lower_path, pi->userid);
	if (err) {
		unlock_dir(lower_parent_dentry);
		goto out;
	}

	fsstack_copy_attr_times(dir, sdcardfs_lower_inode(dir));
	fsstack_copy_inode_size(dir, lower_parent_dentry->d_inode);
	
	set_nlink(dir, sdcardfs_lower_inode(dir)->i_nlink);

	unlock_dir(lower_parent_dentry);

	
	if(need_graft_path(dentry)) {

		err = setup_obb_dentry(dentry, &lower_path);
		if(err) {
			sdcardfs_put_reset_lower_path(dentry);

			path_get(&lower_path);
		} else
			make_nomedia_in_obb = 1;
	}

	if ((!sbi->options.multiuser) && (!strcasecmp(dentry->d_name.name, "obb"))
		&& (pi->perm == PERM_ANDROID) && (pi->userid == 0))
		make_nomedia_in_obb = 1;

	
	if (make_nomedia_in_obb ||
		((pi->perm == PERM_ANDROID) && (!strcasecmp(dentry->d_name.name, "data")))) {
		set_fs_pwd(current->fs, &lower_path);
		touch_err = touch(".nomedia", 0664);
		if (touch_err) {
			printk(KERN_ERR "sdcardfs: failed to create .nomedia in %s: %d\n",
							lower_path.dentry->d_name.name, touch_err);
			goto out;
		}
	}
out:
	mnt_drop_write(lower_path.mnt);
	current->fs = saved_fs;
	free_fs_struct(copied_fs);
out_unlock:
	sdcardfs_put_lower_path(dentry, &lower_path);
out_revert:
	REVERT_CRED(saved_cred);
out_eacces:
	return err;
}

static int sdcardfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower_dentry;
	struct dentry *lower_dir_dentry;
	int err;
	struct path lower_path;
	const struct cred *saved_cred = NULL;

	if(!check_caller_access_to_name(dir, dentry->d_name.name)) {
		printk(KERN_INFO "%s: need to check the caller's gid in packages.list\n"
						 "  dentry: %s, task:%s\n",
						 __func__, dentry->d_name.name, current->comm);
		err = -EACCES;
		goto out_eacces;
	}

	
	OVERRIDE_CRED(SDCARDFS_SB(dir->i_sb), saved_cred);

	sdcardfs_get_real_lower(dentry, &lower_path);

	lower_dentry = lower_path.dentry;
	lower_dir_dentry = lock_parent(lower_dentry);

	err = mnt_want_write(lower_path.mnt);
	if (err)
		goto out_unlock;
	err = vfs_rmdir(lower_dir_dentry->d_inode, lower_dentry);
	if (err)
		goto out;

	d_drop(dentry);	
	if (dentry->d_inode)
		clear_nlink(dentry->d_inode);
	fsstack_copy_attr_times(dir, lower_dir_dentry->d_inode);
	fsstack_copy_inode_size(dir, lower_dir_dentry->d_inode);
	set_nlink(dir, lower_dir_dentry->d_inode->i_nlink);

out:
	mnt_drop_write(lower_path.mnt);
out_unlock:
	unlock_dir(lower_dir_dentry);
	sdcardfs_put_real_lower(dentry, &lower_path);
	REVERT_CRED(saved_cred);
out_eacces:
	return err;
}

#if 0
static int sdcardfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
			dev_t dev)
{
	int err = 0;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;

	OVERRIDE_CRED(SDCARDFS_SB(dir->i_sb));

	sdcardfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = mnt_want_write(lower_path.mnt);
	if (err)
		goto out_unlock;
	err = vfs_mknod(lower_parent_dentry->d_inode, lower_dentry, mode, dev);
	if (err)
		goto out;

	err = sdcardfs_interpose(dentry, dir->i_sb, &lower_path);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, sdcardfs_lower_inode(dir));
	fsstack_copy_inode_size(dir, lower_parent_dentry->d_inode);

out:
	mnt_drop_write(lower_path.mnt);
out_unlock:
	unlock_dir(lower_parent_dentry);
	sdcardfs_put_lower_path(dentry, &lower_path);
	REVERT_CRED();
	return err;
}
#endif

static int sdcardfs_rename(struct inode *old_dir, struct dentry *old_dentry,
			 struct inode *new_dir, struct dentry *new_dentry)
{
	int err = 0;
	struct dentry *lower_old_dentry = NULL;
	struct dentry *lower_new_dentry = NULL;
	struct dentry *lower_old_dir_dentry = NULL;
	struct dentry *lower_new_dir_dentry = NULL;
	struct dentry *trap = NULL;
	struct dentry *new_parent = NULL;
	struct path lower_old_path, lower_new_path;
	const struct cred *saved_cred = NULL;

	if(!check_caller_access_to_name(old_dir, old_dentry->d_name.name) ||
		!check_caller_access_to_name(new_dir, new_dentry->d_name.name)) {
		printk(KERN_INFO "%s: need to check the caller's gid in packages.list\n"
						 "  new_dentry: %s, task:%s\n",
						 __func__, new_dentry->d_name.name, current->comm);
		err = -EACCES;
		goto out_eacces;
	}

	
	OVERRIDE_CRED(SDCARDFS_SB(old_dir->i_sb), saved_cred);

	sdcardfs_get_real_lower(old_dentry, &lower_old_path);
	sdcardfs_get_lower_path(new_dentry, &lower_new_path);
	lower_old_dentry = lower_old_path.dentry;
	lower_new_dentry = lower_new_path.dentry;
	lower_old_dir_dentry = dget_parent(lower_old_dentry);
	lower_new_dir_dentry = dget_parent(lower_new_dentry);

	trap = lock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	
	if (trap == lower_old_dentry) {
		err = -EINVAL;
		goto out;
	}
	
	if (trap == lower_new_dentry) {
		err = -ENOTEMPTY;
		goto out;
	}

	err = mnt_want_write(lower_old_path.mnt);
	if (err)
		goto out;
	err = mnt_want_write(lower_new_path.mnt);
	if (err)
		goto out_drop_old_write;

	err = vfs_rename(lower_old_dir_dentry->d_inode, lower_old_dentry,
			 lower_new_dir_dentry->d_inode, lower_new_dentry);
	if (err)
		goto out_err;

	
	sdcardfs_copy_and_fix_attrs(new_dir, lower_new_dir_dentry->d_inode);
	fsstack_copy_inode_size(new_dir, lower_new_dir_dentry->d_inode);

	if (new_dir != old_dir) {
		sdcardfs_copy_and_fix_attrs(old_dir, lower_old_dir_dentry->d_inode);
		fsstack_copy_inode_size(old_dir, lower_old_dir_dentry->d_inode);

		new_parent = dget_parent(new_dentry);
		if(new_parent) {
			if(old_dentry->d_inode) {
				update_derived_permission_lock(old_dentry);
			}
			dput(new_parent);
		}
	}
	get_derived_permission_new(new_dentry->d_parent, old_dentry, new_dentry);
	fix_derived_permission(old_dentry->d_inode);
	fixup_top_recursive(old_dentry);
out_err:
	mnt_drop_write(lower_new_path.mnt);
out_drop_old_write:
	mnt_drop_write(lower_old_path.mnt);
out:
	unlock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	dput(lower_old_dir_dentry);
	dput(lower_new_dir_dentry);
	sdcardfs_put_real_lower(old_dentry, &lower_old_path);
	sdcardfs_put_lower_path(new_dentry, &lower_new_path);
	REVERT_CRED(saved_cred);
out_eacces:
	return err;
}

#if 0
static int sdcardfs_readlink(struct dentry *dentry, char __user *buf, int bufsiz)
{
	int err;
	struct dentry *lower_dentry;
	struct path lower_path;
	

	sdcardfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	if (!lower_dentry->d_inode->i_op ||
	    !lower_dentry->d_inode->i_op->readlink) {
		err = -EINVAL;
		goto out;
	}

	err = lower_dentry->d_inode->i_op->readlink(lower_dentry,
						    buf, bufsiz);
	if (err < 0)
		goto out;
	fsstack_copy_attr_atime(dentry->d_inode, lower_dentry->d_inode);

out:
	sdcardfs_put_lower_path(dentry, &lower_path);
	return err;
}
#endif

#if 0
static void *sdcardfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	char *buf;
	int len = PAGE_SIZE, err;
	mm_segment_t old_fs;

	
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf) {
		buf = ERR_PTR(-ENOMEM);
		goto out;
	}

	
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = sdcardfs_readlink(dentry, buf, len);
	set_fs(old_fs);
	if (err < 0) {
		kfree(buf);
		buf = ERR_PTR(err);
	} else {
		buf[err] = '\0';
	}
out:
	nd_set_link(nd, buf);
	return NULL;
}
#endif

#if 0
static void sdcardfs_put_link(struct dentry *dentry, struct nameidata *nd,
			    void *cookie)
{
	char *buf = nd_get_link(nd);
	if (!IS_ERR(buf))	
		kfree(buf);
}
#endif

static int sdcardfs_permission(struct inode *inode, int mask)
{
	int err;
	struct inode *top = SDCARDFS_I(inode)->top;

	
	if(top != NULL)
	{
		if (from_kuid(&init_user_ns, inode->i_uid) != from_kuid(&init_user_ns, top->i_uid)) {
			SDCARDFS_I(inode)->d_uid = SDCARDFS_I(top)->d_uid;
			fix_derived_permission(inode);
		}
	}

	err = generic_permission(inode, mask);

#if 0
	if (!err) {
		struct inode *lower_inode;
		OVERRIDE_CRED(SDCARDFS_SB(inode->sb));

		lower_inode = sdcardfs_lower_inode(inode);
		err = inode_permission(lower_inode, mask);

		REVERT_CRED();
	}
#endif
	return err;

}

static int sdcardfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
		 struct kstat *stat)
{
	struct dentry *lower_dentry;
	struct inode *inode;
	struct inode *lower_inode;
	struct path lower_path;
	struct dentry *parent;

	parent = dget_parent(dentry);
	if(!check_caller_access_to_name(parent->d_inode, dentry->d_name.name)) {
		printk(KERN_INFO "%s: need to check the caller's gid in packages.list\n"
						 "  dentry: %s, task:%s\n",
						 __func__, dentry->d_name.name, current->comm);
		dput(parent);
		return -EACCES;
	}
	dput(parent);

	inode = dentry->d_inode;

	sdcardfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_inode = sdcardfs_lower_inode(inode);


	sdcardfs_copy_and_fix_attrs(inode, lower_inode);
	fsstack_copy_inode_size(inode, lower_inode);


	generic_fillattr(inode, stat);
	sdcardfs_put_lower_path(dentry, &lower_path);
	return 0;
}

static int sdcardfs_setattr(struct dentry *dentry, struct iattr *ia)
{
	int err = 0;
	struct dentry *lower_dentry;
	struct inode *inode;
	struct inode *lower_inode;
	struct path lower_path;
	struct iattr lower_ia;
	struct dentry *parent;
	const struct cred *saved_cred = NULL;

	inode = dentry->d_inode;

	err = inode_change_ok(inode, ia);

	
	if (!err) {
		
		parent = dget_parent(dentry);
		if(!check_caller_access_to_name(parent->d_inode, dentry->d_name.name)) {
			printk(KERN_INFO "%s: need to check the caller's gid in packages.list\n"
							 "  dentry: %s, task:%s\n",
							 __func__, dentry->d_name.name, current->comm);
			err = -EACCES;
		}
		dput(parent);
	}

	if (err)
		goto out_err;

	OVERRIDE_CRED(SDCARDFS_SB(inode->i_sb), saved_cred);
	sdcardfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_inode = sdcardfs_lower_inode(inode);

	
	memcpy(&lower_ia, ia, sizeof(lower_ia));
	if (ia->ia_valid & ATTR_FILE)
		lower_ia.ia_file = sdcardfs_lower_file(ia->ia_file);

	lower_ia.ia_valid &= ~(ATTR_UID | ATTR_GID | ATTR_MODE);

	if (current->mm)
		down_write(&current->mm->mmap_sem);
	if (ia->ia_valid & ATTR_SIZE) {
		err = inode_newsize_ok(inode, ia->ia_size);
		if (err) {
			if (current->mm)
				up_write(&current->mm->mmap_sem);
			goto out;
		}
		truncate_setsize(inode, ia->ia_size);
	}

	if (lower_ia.ia_valid & (ATTR_KILL_SUID | ATTR_KILL_SGID))
		lower_ia.ia_valid &= ~ATTR_MODE;

	if (current->mm)
		up_write(&current->mm->mmap_sem);
	
	mutex_lock(&lower_dentry->d_inode->i_mutex);
	if (current->mm)
		down_write(&current->mm->mmap_sem);
	err = notify_change(lower_dentry, &lower_ia); 
	if (current->mm)
		up_write(&current->mm->mmap_sem);
	mutex_unlock(&lower_dentry->d_inode->i_mutex);
	if (err)
		goto out;

	
	sdcardfs_copy_and_fix_attrs(inode, lower_inode);


out:
	sdcardfs_put_lower_path(dentry, &lower_path);
	REVERT_CRED(saved_cred);
out_err:
	return err;
}

const struct inode_operations sdcardfs_symlink_iops = {
	.permission	= sdcardfs_permission,
	.setattr	= sdcardfs_setattr,
};

const struct inode_operations sdcardfs_dir_iops = {
	.create		= sdcardfs_create,
	.lookup		= sdcardfs_lookup,
	.permission	= sdcardfs_permission,
	.unlink		= sdcardfs_unlink,
	.mkdir		= sdcardfs_mkdir,
	.rmdir		= sdcardfs_rmdir,
	.rename		= sdcardfs_rename,
	.setattr	= sdcardfs_setattr,
	.getattr	= sdcardfs_getattr,
};

const struct inode_operations sdcardfs_main_iops = {
	.permission	= sdcardfs_permission,
	.setattr	= sdcardfs_setattr,
	.getattr	= sdcardfs_getattr,
};