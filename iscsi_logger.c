/*********************************************************************************************
 * Copyright (c) 2011, SmApper Technologies Inc
 * $Id: iscsi_logger.c 457 2011-09-14 05:51:21Z hkoehler $
 * Author: Heiko Koehler
 *********************************************************************************************/

#include <linux/socket.h>
#include <linux/netlink.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <net/net_namespace.h>

#include "../../include/netlink_iscsi_logger.h"
#include "iscsi_logger.h"

#define MSG_RND_UP(s) (((s)+0x3) & ~0x3)

/* protect state of logger */
static DEFINE_MUTEX(iscsi_logger_mtx);
/* NetLink socket */
static struct sock *nl_sk = NULL;
/* PID of user space process */
static u32 pid;
/* path of staging directory */
static char staging_dir[MAX_DIR_PATH];
/* temporary buffer */
static char buf[MAX_DIR_PATH+MAX_FILE_NAME+1];
/* max size of log file in pages */
static size_t max_file_size = 0;
/* generation id of file */
static int generation = 0;
/* current log file */
static struct file *log_file = NULL;
/* current log page */
static struct page *log_page = NULL;
/* current page offset in log file */
static pgoff_t poffset = 0;
/* current offset in page */
static off_t offset = 0;
/* currently mapped memory */
static void *paddr = NULL;
/* file system data returned from address operations */
static void *fsdata;
/* name of current log file */
static char log_file_name[MAX_FILE_NAME];

/*
 * unit test logger
 */
static void iscsi_logger_test(void)
{
	struct iscsi_log_msg *msg;
	int i, size;

	iscsi_logger_start();
	for (i = 0; i < 1000000; i++) {
		//if (i % 10000 == 0)
		//	iscsi_logger_roll();
		size = i % 1024 + 10;
		msg = iscsi_logger_alloc(ISCSI_LOG_MSG_SIZE(size), ISCSI_RAW_MSG);
		if (msg == NULL)
			break;
		sprintf(msg->data, "%x,%x", i, size);
		iscsi_logger_commit();
	}
	iscsi_logger_roll();
	iscsi_logger_stop();
}

/*
 * set current log file
 * roll over to new log file if maximum size has been reached
 */
static inline void fetch_log_file(void)
{
	if (likely(log_file)) {
		if (poffset < max_file_size)
			return;
		iscsi_logger_roll();
	}

	sprintf(log_file_name, "%10lu.%03d", get_seconds(), generation++ % 1000);
	sprintf(buf, "%s/%s", staging_dir, log_file_name);
	log_file = filp_open(buf, O_RDWR|O_CREAT, 0);
	if (IS_ERR_OR_NULL(log_file))
		log_file = NULL;

	if (log_file)
		printk(KERN_INFO "%s: created new log file %s\n", __func__, buf);
	else
		printk(KERN_INFO "%s: failed to create log file %s\n", __func__, buf);
}

/*
 * put current log page
 */
static void put_log_page(void)
{
	unsigned char *addr;
	struct address_space *mapping;
	long status = 0;

	if (unlikely(log_page == NULL))
		return;

	/* terminate page */
	if (offset < PAGE_SIZE) {
		paddr = kmap_atomic(log_page, KM_USER0);
		addr = paddr + offset;
		*addr = ISCSI_TERMINATOR_MSG;
		kunmap_atomic(paddr, KM_USER0);
	}

	mapping = log_file->f_mapping;
	status = pagecache_write_end(log_file, mapping, poffset << PAGE_SHIFT,
			PAGE_SIZE, PAGE_SIZE, log_page, fsdata);
	if (unlikely(status < 0))
		printk(KERN_ERR "%s: write_end() failed with %ld\n",
				__func__, status);
	log_page = NULL;
	fsdata = NULL;
}

/*
 * get current log page
 * switch to next page
 */
static void fetch_log_page(size_t size)
{
	struct address_space *mapping;
	long status = 0;

	if (likely(log_page)) {
		/* does record fit into current page? */
		if (offset + size <= PAGE_SIZE)
			return;
		put_log_page();
		offset = 0;
		/* fetch next page */
		poffset++;
	}

	/* get current log file */
	fetch_log_file();
	if (log_file == NULL)
		return;

	/* fetch next page */
	mapping = log_file->f_mapping;
	status = pagecache_write_begin(log_file, mapping, poffset << PAGE_SHIFT,
			PAGE_SIZE, AOP_FLAG_UNINTERRUPTIBLE, &log_page, &fsdata);
	if (unlikely(status))
		printk(KERN_ERR "%s: write_begin() failed with %ld\n",
				__func__, status);
}

/*
 * allocate log message from staging buffer
 */
struct iscsi_log_msg *
iscsi_logger_alloc(size_t size, uint8_t type)
{
	struct iscsi_log_msg *msg;
	char *addr;

	size = MSG_RND_UP(size);
	BUG_ON(size > PAGE_SIZE);
	fetch_log_page(size);
	if (log_page == NULL)
		return NULL;
	paddr = kmap_atomic(log_page, KM_USER0);
	addr = paddr + offset;
	offset += size;
	msg = (struct iscsi_log_msg *)addr;
	msg->size = size;
	msg->type = type;

	return msg;
}

/*
 * commit log message
 */
void iscsi_logger_commit(void)
{
	BUG_ON(!paddr);
	kunmap_atomic(paddr, KM_USER0);
}

/*
 * roll-over to next log file
 */
void iscsi_logger_roll(void)
{
	if (log_file) {
		put_log_page();
		filp_close(log_file, NULL);
		poffset = offset = 0;
		log_file = NULL;
	}
}

/*
 * start logging
 */
void iscsi_logger_start(void)
{
	mutex_lock(&iscsi_logger_mtx);
	/* get current log file */
	fetch_log_file();
}

/*
 * stop logging
 */
void iscsi_logger_stop(void)
{
	put_log_page();
	mutex_unlock(&iscsi_logger_mtx);
}

/*
 * register staging directory
 */
int nl_register_dir(struct sk_buff *skb)
{
	struct nl_msg *msg;
	struct nlmsghdr *nlh;

	nlh = (struct nlmsghdr *)skb->data;
	msg = NLMSG_DATA(nlh);
	msg->reg_msg.dir[MAX_DIR_PATH-1] = 0;

	mutex_lock(&iscsi_logger_mtx);
	strncpy(staging_dir, msg->reg_msg.dir, MAX_DIR_PATH);
	pid = nlh->nlmsg_pid;
	max_file_size = msg->reg_msg.max_size >> PAGE_SHIFT;
	mutex_unlock(&iscsi_logger_mtx);

	printk(KERN_INFO "%s: max file size = %d (%d pages), path = %s\n",
			__func__, (int)msg->reg_msg.max_size, (int)max_file_size, msg->reg_msg.dir);

	return 0;
}

/*
 * unit test logger
 */
int nl_test(struct sk_buff *skb)
{
	iscsi_logger_test();
	return 0;
}

/*
 * NetLink socket call-back
 */
static void nl_input(struct sk_buff *skb)
{
	struct nl_msg *msg;
	struct nlmsghdr *nlh;
	int err = 0;

	nlh = (struct nlmsghdr *)skb->data;
	if (nlh->nlmsg_len < sizeof(*nlh) + sizeof(struct nl_msg)) {
		printk(KERN_ERR "%s: message truncated\n", __func__);
		netlink_ack(skb, nlh, -EINVAL);
		return;
	}
	msg = NLMSG_DATA(nlh);
	if (msg->version != ISCSI_LOGGER_API_VER) {
		printk(KERN_ERR "%s: wrong version %d, current=%d\n", __func__,
				msg->version, ISCSI_LOGGER_API_VER);
		netlink_ack(skb, nlh, -EINVAL);
		return;
	}
	printk(KERN_INFO "%s: pid=%d seq=%d type=%d skb->len=%d\n", __func__,
			nlh->nlmsg_pid, nlh->nlmsg_seq, nlh->nlmsg_type, skb->len);

	switch (nlh->nlmsg_type) {
	case ISCSI_LOGGER_REGISTER:
		err = nl_register_dir(skb);
		break;
	case ISCSI_LOGGER_TEST:
		err = nl_test(skb);
		break;
	default:
		printk(KERN_ERR "%s: unknown message type %d\n", __func__, nlh->nlmsg_type);
	}

	netlink_ack(skb, nlh, err);
	skb_pull(skb, nlh->nlmsg_len);
}

/*
 * bring up netlink socket
 */
int iscsi_logger_init(void)
{
	nl_sk = netlink_kernel_create(&init_net, NETLINK_UNUSED, 0,
			nl_input, NULL, THIS_MODULE);
	if (nl_sk == NULL)
		return -EIO;
	strcpy(staging_dir, "/dev/shm");
	pid = 0;
	max_file_size = 1024;
	return 0;
}

/*
 * shutdown netlink socket
 */
void iscsi_logger_exit(void)
{
	mutex_lock(&iscsi_logger_mtx);
	if (log_file)
		filp_close(log_file, NULL);
	netlink_kernel_release(nl_sk);
	mutex_unlock(&iscsi_logger_mtx);
}
