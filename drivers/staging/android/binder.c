/* binder.c
 *
 * Android IPC Subsystem
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <asm/cacheflush.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nsproxy.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include "binder.h"

static DEFINE_MUTEX(binder_lock);
static HLIST_HEAD(binder_procs);
static struct binder_node *binder_context_mgr_node;
static uid_t binder_context_mgr_uid = -1;
static int binder_last_id;
static struct proc_dir_entry *binder_proc_dir_entry_root;
static struct proc_dir_entry *binder_proc_dir_entry_proc;
static struct hlist_head binder_dead_nodes;
static HLIST_HEAD(binder_deferred_list);
static DEFINE_MUTEX(binder_deferred_lock);

static int binder_read_proc_proc(
	char *page, char **start, off_t off, int count, int *eof, void *data);

/* This is only defined in include/asm-arm/sizes.h */
#ifndef SZ_1K
#define SZ_1K                               0x400
#endif

#ifndef SZ_4M
#define SZ_4M                               0x400000
#endif

#define FORBIDDEN_MMAP_FLAGS                (VM_WRITE)

#define BINDER_SMALL_BUF_SIZE (PAGE_SIZE * 64)

enum {
	BINDER_DEBUG_USER_ERROR             = 1U << 0,
	BINDER_DEBUG_FAILED_TRANSACTION     = 1U << 1,
	BINDER_DEBUG_DEAD_TRANSACTION       = 1U << 2,
	BINDER_DEBUG_OPEN_CLOSE             = 1U << 3,
	BINDER_DEBUG_DEAD_BINDER            = 1U << 4,
	BINDER_DEBUG_DEATH_NOTIFICATION     = 1U << 5,
	BINDER_DEBUG_READ_WRITE             = 1U << 6,
	BINDER_DEBUG_USER_REFS              = 1U << 7,
	BINDER_DEBUG_THREADS                = 1U << 8,
	BINDER_DEBUG_TRANSACTION            = 1U << 9,
	BINDER_DEBUG_TRANSACTION_COMPLETE   = 1U << 10,
	BINDER_DEBUG_FREE_BUFFER            = 1U << 11,
	BINDER_DEBUG_INTERNAL_REFS          = 1U << 12,
	BINDER_DEBUG_BUFFER_ALLOC           = 1U << 13,
	BINDER_DEBUG_PRIORITY_CAP           = 1U << 14,
	BINDER_DEBUG_BUFFER_ALLOC_ASYNC     = 1U << 15,
};
static uint32_t binder_debug_mask = BINDER_DEBUG_USER_ERROR |
	BINDER_DEBUG_FAILED_TRANSACTION | BINDER_DEBUG_DEAD_TRANSACTION;
module_param_named(debug_mask, binder_debug_mask, uint, S_IWUSR | S_IRUGO);
static int binder_debug_no_lock;
module_param_named(proc_no_lock, binder_debug_no_lock, bool, S_IWUSR | S_IRUGO);
static DECLARE_WAIT_QUEUE_HEAD(binder_user_error_wait);
static int binder_stop_on_user_error;
static int binder_set_stop_on_user_error(
	const char *val, struct kernel_param *kp)
{
	int ret;
	ret = param_set_int(val, kp);
	if (binder_stop_on_user_error < 2)
		wake_up(&binder_user_error_wait);
	return ret;
}
module_param_call(stop_on_user_error, binder_set_stop_on_user_error,
	param_get_int, &binder_stop_on_user_error, S_IWUSR | S_IRUGO);

#define binder_user_error(x...) \
	do { \
		if (binder_debug_mask & BINDER_DEBUG_USER_ERROR) \
			printk(KERN_INFO x); \
		if (binder_stop_on_user_error) \
			binder_stop_on_user_error = 2; \
	} while (0)

enum {
	BINDER_STAT_PROC,
	BINDER_STAT_THREAD,
	BINDER_STAT_NODE,
	BINDER_STAT_REF,
	BINDER_STAT_DEATH,
	BINDER_STAT_TRANSACTION,
	BINDER_STAT_TRANSACTION_COMPLETE,
	BINDER_STAT_COUNT
};

struct binder_stats {
	int br[_IOC_NR(BR_FAILED_REPLY) + 1];
	int bc[_IOC_NR(BC_DEAD_BINDER_DONE) + 1];
	int obj_created[BINDER_STAT_COUNT];
	int obj_deleted[BINDER_STAT_COUNT];
};

static struct binder_stats binder_stats;

struct binder_transaction_log_entry {
	int debug_id;
	int call_type;
	int from_proc;
	int from_thread;
	int target_handle;
	int to_proc;
	int to_thread;
	int to_node;
	int data_size;
	int offsets_size;
};
struct binder_transaction_log {
	int next;
	int full;
	struct binder_transaction_log_entry entry[32];
};
struct binder_transaction_log binder_transaction_log;
struct binder_transaction_log binder_transaction_log_failed;

static struct binder_transaction_log_entry *binder_transaction_log_add(
	struct binder_transaction_log *log)
{
	struct binder_transaction_log_entry *e;
	e = &log->entry[log->next];
	memset(e, 0, sizeof(*e));
	log->next++;
	if (log->next == ARRAY_SIZE(log->entry)) {
		log->next = 0;
		log->full = 1;
	}
	return e;
}

/**
 * @brief 用于描述待处理的工作项。
 * <p> 
 * 这个工作项可能属于一个进程，也有可能属于一个进程中的某个线程。
 * 
 */
struct binder_work {
	//用来将该结构体嵌入到一个宿主结构中
	struct list_head entry;
	//用来描述工作项的类型。根据成员变量type取值，Binder驱动程序就可以判断出一个binder_work结构体嵌入到什么类型的宿主结构中
	enum {
		BINDER_WORK_TRANSACTION = 1,
		BINDER_WORK_TRANSACTION_COMPLETE,
		BINDER_WORK_NODE,
		BINDER_WORK_DEAD_BINDER,
		BINDER_WORK_DEAD_BINDER_AND_CLEAR,
		BINDER_WORK_CLEAR_DEATH_NOTIFICATION,
	} type;
};

/**
 * @brief 用来描述一个Binder实体对象。
 * 
 * 每一个Service组件都在Binder驱动程序中对应一个Binder实体对象，用来描述它在内核中的状态。Binder驱动程序通过强引用计数和若引用计数
 * 技术来维护他们的生命周期。
 * 
 */
struct binder_node {
	//标志一个Binder实体对象的身份，用来帮助调试Binder驱动程序
	int debug_id;
	
	//指向一个Binder实体对象的宿主进程。在Binder驱动程序中，这些宿主进程通过一个binder_proc结构体来描述。
	//宿主进程使用一个红黑树来维护它内部所有的Binder实体对象，而每一个Binder实体对象的成员变量rb_node就正好是
	//这个红黑树中的一个节点。如果一个Binder实体对象的宿主进程已经死亡了，那么这个Binder实体对象就会通过他的
	//成员变量dead_node保存在一个全局的hash列表中。对应到代码中，使用了一个union标识这两种互斥的状态。
	struct binder_proc *proc;
	union {
		struct rb_node rb_node;
		struct hlist_node dead_node;
	};

	//由于一个Binder实体对象可能会被多个Client组件引用，因此，Binder驱动程序就使用结构体binder_ref来描述这些引用关系，
	//并且将引用了同一个Binder实体对象的所有引用都保存在一个hash列表中。这个hash列表通过Binder实体对象的成员变量refs来描述，
	//而Binder驱动程序通过这个成员变量就可以知道有哪些Client组件引用了同一个Binder实体对象
	struct hlist_head refs;

	//成员变量internal_strong_refs和 local_strong_refs均是用来描述一个Binder实体对象的强引用计数
	int internal_strong_refs;
	int local_strong_refs;
	//local_weak_refs用于描述一个Binder实体对象的弱引用计数
	int local_weak_refs;

	//当一个Binder实体对象请求一个Service组件来执行某一个操作时，会增加该Service组件的强引用计数或者弱引用计数，相应地，
	//Binder实体对象会将其成员变量has_strong_ref和has_weak_ref的值设置为1.当一个Service组件完成一个Binder实体对象
	//所请求的操作后，Binder实体对象就会请求减少该Service组件的强引用计数或者弱引用计数。
	unsigned has_strong_ref : 1;
	unsigned has_weak_ref : 1;
	//Binder实体对象在请求一个Service组件增加或者减少 强/弱引用计数的过程中，会将其成员变量pending_strong_ref或者
	//pending_weak_ref的值设置为1，而当该Service组件增加或者减少了强/弱引用计数之后，Binder实体对象就会将这两个成员变量
	//的值置为0
	unsigned pending_strong_ref : 1;
	unsigned pending_weak_ref : 1;
	
	//prt和cookie分别指向一个用户空间地址，他们用来描述用户空间中一个Service组件
	//cookie指向该Service组件的地址
	//__user 表示是一个用户空间的指针，所以kernel不能直接使用（如copy等）。要使用copy_from_user，copy_to_user等函数进行操作
	//详细参见：https://blog.csdn.net/q345852047/article/details/7710818
	//另外 void *cookie 是void指针，参见：https://www.runoob.com/w3cnote/c-void-intro.html
	void __user *cookie;
	//指向该Service内部的一个引用计数对象（类型为weakref_impl）的地址
	void __user *ptr;
	
	//用于描述一个Binder实体对象是否正在处理一个异步事务，如果是就等于1，否则等于0 。一般情况下，Binder驱动程序都是将一个事务
	//保存在一个线程的一个todo队列中，表示要由该线程来处理该事务。每一个事务都关联着一个Binder实体对象，表示该事务的目标处理对象
	//即要求与该Binder实体对象对应的Service组件在指定的线程中处理该事务。然而，当Binder驱动程序发现一个事务是异步事务时，就会将
	//它保存在目标Binder实体对象的一个异步事务队列中，这个异步事务队列就是由该目标Binder实体对象的成员变量async_todo来描述。
	//异步事务的定义是那些单向的进程间通信请求，即不需要等待应答的进程间通信请求，与此相对的便是同步事务。因为不需要等待应答，
	//Binder驱动程序就会认为异步事务的优先级低于同步事务，具体就表现为在同一时刻，一个Binder实体对象的所有异步事务至多只有一个会得到处理，
	//其余的都等待在异步事务队列中，而同步则没有这个限制。
	unsigned has_async_transaction : 1;
	struct list_head async_todo;

	//当一个Binder实体对象的引用计数由0变成1，或者由1变成0时，Binder驱动程序就会请求相应的Service组件增加或者减少其引用计数。
	//这时候Binder驱动程序就会将引用计数修改封装成一个类型为binder_node的工作项，即将一个Binder实体对象的成员变量work的值
	//设置为BINDER_WORK_NODE，并且将它添加到相应进程的todo队列中去等待处理。
	struct binder_work work;

	//用来描述一个Binder实体对象是否可以接受包含有文件描述符的进程间通信数据。如果他的值等于1，就表示可以接受，否则，就表示禁止接受。
	//当一个进程向另外一个进程发送的数据中包含有文件描述符时，Binder驱动程序就会自动在目标进程中打开一个相同的文件。基于安全性考虑，
	//Binder驱动程序就要通过成员变量accept_fds来防止源进程在目标进程中打开文件。
	unsigned accept_fds : 1;

	//表示一个Binder实体对象在处理一个来自Client进程的请求时，他所要求的处理线程，即Service进程中的一个线程，应该具备的最小优先级，
	//这样就保证了与该Binder实体对象对应的Service组件可以在一个具有一定优先级的线程中处理一个来自Client进程的通信请求。一个Service
	//组件将自己注册到Binder驱动程序时，可以指定这个最小线程优先级，而Binder驱动程序会把这个最小线程优先级保存在相应的Binder实体对象
	//的成员变量min_priority中
	int min_priority : 8;
	
};

/**
 * @brief 描述一个Service组件的死亡接收通知
 * 在正常情况下，一个Service组件被其他Client进程引用时，他是不可销毁的。然而，Client进程是无法控制它所引用的Service组件的生命周期的，
 * 因为Service组件所在的进程可能会意外的崩溃，从而导致他意外的死亡。一个折中的处理方法是，Client进程能够在他所引用的Service组件死亡时
 * 获得通知，以便可以做出相应的处理。这时候Client进程就需要将一个用来接收死亡通知的对象的地址注册到Binder驱动程序中。
 * 
 * Binder驱动程序决定要向一个Client进程发送一个Service组件死亡通知时，会将一个binder_ref_death结构体封装成一个工作项，并根据实际情况
 * 来设置该结构体的值，最后将这个工作项添加到Client进程的todo队列中去等待处理。
 * 
 * 下面两种情况，Binder驱动会向Client进程发送一个Service组件的死亡通知
 * （1）当Binder驱动程序监测到一个Service组件死亡时，他就会找到该Service组件对应的Binder实体对象，然后通过Binder实体对象的成员变量
 * 		refs就可以找到所有引用了他的Client进程，最后就找到这些Client进程注册的死亡接收通知，即一个binder_ref_death结构体。这时候
 * 		Binder驱动程序就会将该binder_ref_death结构体添加到Client进程的todo队列中去等待处理。在这种情况下，Binder驱动程序将死亡通知
 * 		的类型设置为BINDER_WORK_DEAD_BINDER。
 * 
 * （2）当Client进程向Binder驱动程序注册一个死亡接收通知时，如果他所引用的Service组件已经死亡，那么Binder驱动程序就会马上发送一个死亡通知
 * 		给该Client进程。在这种情况下，Binder驱动程序也会将死亡通知类型设置为BINDER_WORK_DEAD_BINDER
 * 
 * 另外，当Client进程向Binder驱动程序注销一个死亡接收通知时，Binder驱动程序也会向该Client进程的todo队列中发送一个类型为binder_ref_death
 * 的工作项，用来表示注销结果。这时候也会分为两种情况来考虑。
 * （1）如果Client进程在注销一个死亡接收通知时，相应的Service组件还没有死亡，那么Binder驱动程序就会找到之前所注册的一个binder_ref_death
 * 		结构体并且将他的类型work修改为BINDER_WORK_CLEAR_DEATH_NOTIFICATION，然后再将该binder_ref_death结构体封装成一个工作项添加到
 * 		该Client进程的todo队列中去等待处理
 * 
 * （2）如果Client进程在注销一个死亡接收通知时，相应的Service组件已经死亡，那么Binder驱动程序就会找到之前所注册的binder_ref_death结构体
 * 		并且将他的类型work修改为BINDER_WORK_DEAD_BINDER_AND_CLEAR，然后再将该binder_ref_death结构体封装成一个工作项添加到该Client
 * 		进程的todo队列中去等待处理。
 * 
 * Client进程在处理这个工作项时，通过对应的binder_ref_death结构体的成员变量work就可以区分注销结果了，即他所引用的Service组件是否已经死亡
 */
struct binder_ref_death {
	//work的取值为BINDER_WORK_DEAD_BINDER、BINDER_WORK_CLEAR_DEATH_NOTIFICATION或者BINDER_WORK_DEAD_BINDER_AND_CLEAR，
	//用来标志一个具体的死亡通知
	struct binder_work work;
	//cookie用来保存负责接收死亡通知的对象的地址
	void __user *cookie;
};

/**
 * @brief binder_ref用来描述一个Binder引用对象。
 * 
 * 每一个Client组件在Binder驱动程序中都对应一个Binder引用对象，用来描述他在内核中的状态。Binder驱动程序通过强引用计数和弱引用计数
 * 技术来维护他们的生命周期。
 * 
 */
struct binder_ref {
	/* Lookups needed: */
	/*   node + proc => ref (transaction) */
	/*   desc + proc => ref (transaction, inc/dec ref) */
	/*   node => refs + procs (proc exit) */
	//标识一个Binder引用对象的身份，用来帮助调试Binder驱动程序。
	int debug_id;

	//node用来描述一个Binder对象所引用的Binder实体对象。前面介绍binder_node时提到，每一个Binder实体对象都有一个hash列表，用来保存
	//那些引用了他的Binder引用对象，而这些Binder引用对象的成员变量node_entry正好是这个hash列表的节点
	struct binder_node *node;
	//Binder实体对象Binder引用对象hash列表中代表当前Binder引用对象的节点
	struct hlist_node node_entry;

	//一个句柄值，或者称为描述符，他是用来描述一个Binder引用对象的。在Client进程的用户空间中，一个Binder引用对象是使用一个句柄值来描述的。
	//因此，当Client进程的用户空间通过Binder驱动程序来访问一个Service组件时，他只需要指定一个句柄值，Binder驱动程序就可以通过该句柄值
	//找到对应的Binder引用对象，然后再根据该Binder应用对象的成员变量node找到对应的Binder实体对象，最后就可以通过该Binder实体对象找到
	//要访问的Service组件
	//注意：一个Binder引用对象的句柄值在进程范围内是唯一的，因此，在两个不同的进程中，同一个句柄值可能代表的是两个不同的目标Service组件。
	uint32_t desc;

	//proc指向一个Binder引用对象的宿主进程。
	struct binder_proc *proc;

	//一个宿主进程使用两个红黑树来保存它内部所有的Binder引用对象。他们分别以句柄值和对应的Binder实体对象的地址来作为关键字
	//保存这些Binder引用对象，而这些Binder引用对象的成员变量rb_node_desc和rb_node_node就正好是这两个红黑树中的节点
	struct rb_node rb_node_desc;
	struct rb_node rb_node_node;
	
	//Binder驱动程序通过强/弱引用计数器来维护一个Binder引用对象的生命周期
	//Binder引用对象的强引用计数器
	int strong;
	//Binder引用对象的弱引用计数器
	int weak;

	//指向一个Service组件的死亡接收通知。当Client进程向Binder驱动程序注册一个他所引用的Service组件的死亡接收通知时，
	//Bidner驱动程序就会创建一个binder_ref_death结构体，然后保存在对应的Binder引用对象的成员变量death中。
	struct binder_ref_death *death;
};

/**
 * @brief 用来描述一个内核缓冲区，用于在进程间传输数据。
 * 
 */
struct binder_buffer {
	//每一个使用Binder进程间通信机制的进程在Binder驱动程序中都有一个内核缓冲区列表。用来保存Binder驱动程序为他所分配的内核缓冲区。
	//entry就是这个内核缓冲区列表的一个节点。
	struct list_head entry; /* free and allocated entries by addesss */
	//进程使用红黑树来分别保存那些正在使用的内核缓冲区和空闲的内核缓冲区。通过另一个成员变量free表示是否是空闲的。
	struct rb_node rb_node; /* free entry by size or allocated entry */
				/* by address */
	//如果一个内核缓冲区是空闲的，free == 1
	unsigned free : 1;

	//成员变量transaction和target_node用来描述一个内核缓冲区正在交给哪一个事务以及哪一个Binder实体对象使用。
	//Binder驱动程序使用一个binder_transaction结构体来描述一个事务，每一个事务都关联有一个目标Binder实体对象。
	//Binder驱动程序将事务数据保存在一个内核缓冲区中，然后将它交给目标Binder实体对象处理，而目标Binder实体对象
	//再将该内核缓冲区的内容交给相应的Service组件处理。Service组件处理完成该事务后，如果发现传递给他的内核缓冲区
	//的成员变量allow_user_free的值为1，那么该Service组件就会请求Binder驱动程序释放该内核缓冲区。
	struct binder_transaction *transaction;
	struct binder_node *target_node;

	//是否允许释放该内存缓冲区
	unsigned allow_user_free : 1;

	//表示与该内核缓冲区关联的是否是一个异步事务，是则为1，否则为0。
	//Binder驱动程序限制了分配给异步事务的内核缓冲区大小，这样做的目的是为了保证同步事务可以优先得到内核缓冲区，
	//以便可以快速地对该同步事务进行处理。
	unsigned async_transaction : 1;

	//用来标志一个内核缓冲区身份，帮助调试Binder驱动程序
	unsigned debug_id : 29;

	//指向一块大小可变的数据缓冲区，他是真正用来保存通信数据的。数据缓冲区保存的数据划分为两种类型，
	//其中一种是普通数据，另一种是Binder对象。Binder驱动程序不关心数据缓冲区中的普通数据，但是必须知道
	//里面的Binder对象，因为他需要根据他们来维护内核中的Binder实体对象和Binder引用对象的生命周期。
	//例如，如果数据缓冲区中包含一个Binder引用，并且该数据缓冲区是传递给另外一个进程的，那么Binder驱动程序
	//就需要为另外一个进程创建一个Binder引用对象，并且增加相应的Binder实体对象的引用计数，因为他也被另外的这个进程
	//引用了。由于数据缓冲区中普通数据和Binder对象是混合在一起保存的，他们之间并没有固定的顺序，因此Binder驱动程序
	//就需要额外的数据来找到里面的Binder对象。在数据缓冲区的后面，有一个偏移数组，它记录了数据缓冲区中每一个Binder对象
	//在数据缓冲区中的位置。偏移数组的大小保存在成员变量offset_size中，而数据缓冲区的大小保存在data_size中
	uint8_t data[0];
	//数据缓冲区的大小
	size_t data_size;
	//记录缓冲区中Binder对象的偏移位置
	size_t offsets_size;
	
};

/**
 * @brief 进程可延迟执行的工作项
 * 
 */
enum {
	//Binder驱动程序为进程分配内核缓冲区时，会为这个内核缓冲区创建一个文件描述符，进程可以通过这个文件描述符
	//将该内核缓冲区映射到自己的地址空间。当进程不再需要使用Binder进程间通信机制时，他就会通知Binder驱动程序关闭该
	//文件描述符，并且释放之前所分配的内核缓冲区。由于这不是一个马上就需要完成的操作，因此Binder驱动程序就会创建一个
	//BINDER_DEFERRED_PUT_FILES类型的工作项来延迟执行该操作。
	BINDER_DEFERRED_PUT_FILES    = 0x01,
	//Binder线程池中的空闲Binder线程是睡眠在一个等待队列中的，进程可以通过调用函数flush来唤醒这些线程，以便他们可以检查
	//进程是否有新的工作项需要处理。这时候Binder驱动程序就会创建一个BINDER_DEFERRED_FLUSH类型的工作项，以便可以延迟执行
	//唤醒空闲Binder线程的操作。
	BINDER_DEFERRED_FLUSH        = 0x02,
	//当进程不再使用Binder进程间通信机制时，他就回调用函数close来关闭设备文件/dev/binder，这时候Binder驱动程序就会释放之前
	//为他分配的资源，例如，释放进程结构体binder_proc、Binder实体对象结构体binder_node以及Binder引用对象结构体binder_ref
	//等。由于资源释放是一个比较耗时的操作，因此，Binder驱动程序会创建一个BINDER_DEFERRED_RELEASE类型的事务来延迟执行他们
	BINDER_DEFERRED_RELEASE      = 0x04,
};

/**
 * @brief 描述一个正在使用Binder进程间通信机制的进程。
 * 当一个进程调用函数open来打开设备文件/dev/binder时，Binder驱动程序就会为他创建一个binder_proc结构体，并且将他保存在一个全局的
 * hash列表中，而成员变量proc_node就正好时该hash列表中的一个节点。
 * 
 */
struct binder_proc {
	//保存当前binder_proc的一个节点
	struct hlist_node proc_node;

	//进程的进程组ID
	int pid;
	//任务控制块
	struct task_struct *tsk;
	//打开文件结构体数组
	struct files_struct *files;
	
	//Binder驱动程序为进程分配的内核缓冲区大小保存在成员变量buffer_size中
	size_t buffer_size;
	//内核空间地址，Binder驱动程序内部使用。
	void *buffer;
	//用户空间地址，应用程序进程内部使用
	struct vm_area_struct *vma;
	//内核空间地址和用户空间地址的差值。这样，给定一个用户空间地址或者一个内核空间地址，Binder驱动程序就可以计算出另外一个地址大小
	ptrdiff_t user_buffer_offset;
	//内核空间地址和用户空间地址都是虚拟地址，他们对应的物理页面保存在成员变量pages中。成员变量pages是类型为 struc page*的一个数组，
	//数组中的每一个元素都执行一个物理页面。Binder驱动程序一开始只为该内核分配一个物理页面，后面不够使用时，再继续分配
	//struct page **pages 代表一个page的二维数组
	struct page **pages;

	//buffer指向一块大的内核缓冲区，Binder驱动程序为了方便对他进行管理，会将他划分为若干个小块。
	//这些小块的内核缓冲区就是使用前面介绍的binder_buffer来描述的，他们保存在一个列表中，按照地址值从小到大的顺序来排列。成员变量buffers
	//便指向该列表的头部。
	struct list_head buffers;
	//列表中的小块内核缓冲区（binder_buffer）有的是正在使用的，即已经分配了物理页面；有的是空闲的，即还没有分配物理页面，他们分别组织在两个红黑树中，
	//free_buffers：保存空闲binder_buffer的红黑树
	struct rb_root free_buffers;
	//allocated_buffers：保存已分配binder_buffer的红黑树
	struct rb_root allocated_buffers;

	//保存了空闲内核缓冲区的大小
	uint32_t buffer_free;
	//保存了当前可以用来保存异步事务数据的内核缓冲区的大小
	size_t free_async_space;

	//每一个使用了Binder进程间通信机制的进程都有一个Binder线程池，用来处理进程间通信请求，这个Binder线程池是由Binder驱动程序来维护的。
	//threads是一个红黑树的根节点，他以线程ID作为关键字来组织一个进程的Binder线程池。进程可以调用函数ioctl将一个线程注册到Binder驱动程序中，
	//同时，当进程没有足够的空闲线程处理进程间通信请求时，Binder驱动程序也可以主动要求进程注册更多线程到Binder线程池中。Binder驱动程序最多
	//可以主动要求进程注册的线程数量保存在成员变量max_threads中，而成员变量ready_threads表示进程当前的空闲Binder线程数目。
	struct rb_root threads;
	//Binder驱动程序可以主动要求进程注册的线程最大数量。max_threads并不是表示Binder线程池中的最大线程数目，进程本身可以主动注册任意数目
	//的线程到Binder线程池中。
	int max_threads;
	//Binder驱动程序每一次主动请求进程注册一个线程时，都会将成员变量requested_threads的值+1
	//而当进程响应这个请求之后，Binder驱动程序就会将成员变量requested_threads的值-1，而将requested_threads_starrted的值+1，
	//表示Binder驱动程序已经主动请求进程注册了多少个线程到Bidner线程池中。
	int requested_threads;
	int requested_threads_started;
	//当前进程空闲Binder线程数目
	int ready_threads;

	//当进程接收到一个进程间通信请求时，Binder驱动程序就将该请求封装成为一个工作项，并且加入到进程的待处理工作项队列中，这个队列用成员变量
	//todo来描述
	struct list_head todo;
	//Binder线程池中空闲的Binder线程会睡眠在由成员变量wait所描述的一个等待队列中，当他们的宿主进程的待处理工作项队列增加了新的工作项之后
	//Binder驱动程序就会唤醒这些线程，以便他们可以去处理新的工作项。
	wait_queue_head_t wait;
	//初始化进程的优先级。但一个线程处理一个工作项时，他的线程优先级可能会被设置为其宿主进程的优先级，即设置为成员变量default_priority的值，
	//这是由于线程是代表其宿主进程来处理一个工作项。线程在处理一个工作项时的优先级还会收到其他因素的影响。
	long default_priority;

	//一个进程内部包含一系列的Binder实体对象和Binder引用对象，进程使用三个红黑树来组织他们
	//nodes红黑树用来组织Binder实体对象，他以Binder实体对象的成员变量ptr作为关键字
	struct rb_root nodes;
	//refs_by_desc和refs_by_node所描述的红黑树是用来组织Binder引用对象的
	//refs_by_desc以Binder引用对象的成员变量desc作为关键字
	struct rb_root refs_by_desc;
	//refs_by_node以Binder引用对象的成员变量node作为关键字
	struct rb_root refs_by_node;

	//deferred_work_node是一个hash列表，用来保存进程可以延迟执行的工作项。这些工作项有三种类型：
	//BINDER_DEFERRED_PUT_FILES、BINDER_DEFERRED_FLUSH和BINDER_DEFERRED_RELEASE
	//Binder驱动程序将所有延迟执行的工作保存在一个hash列表中。如果一个进程有延迟执行的工作项，
	//那么deferred_work_node就刚好是该hash列表中的一个节点，并且使用成员变量deferred_work
	//来描述该延迟工作项的具体类型
	struct hlist_node deferred_work_node;
	//延迟工作项的具体类型
	int deferred_work;

	//用于统计进程数据的，例如，进程间接受到的进程间通信请求次数。
	struct binder_stats stats;
	//当一个进程引用的Service组件死亡时，Binder驱动程序就会向该进程发送一个死亡通知。这个正在发出的
	//死亡通知被封装成一个BINDER_WORK_DEAD_BINDER或者BINDER_WORK_DEAD_BINDER_AND_CLEAR的工作项，
	//并且保存在由成员变量delivered_death所描述的一个队列中，表示驱动程序正在向进程发送的死亡通知
	//当进程接收到这个死亡通知之后，他便会通知Binder驱动程序，这时候Binder驱动程序就会将对应的工作项
	//从成员变量delivered_death所描述的队列中删除。
	struct list_head delivered_death;
};

/**
 * @brief Binder线程状态取值
 * 
 */
enum {
	//一个线程注册到Binder驱动程序之后，他接着就会通过BC_REGISTER_LOOPER或者BC_ENTER_LOOPER
	//协议来通知Binder驱动程序，他可以处理进程间通信请求了。这时候Binder驱动程序就会将他的状态设置为
	//BINDER_LOOPER_STATE_REGISTERED或者BINDER_LOOPER_STATE_ENTERED
	//如果一个线程是应用程序主动注册的，那么他就通过BC_ENTER_LOOPER协议来通知Binder驱动程序，他已经
	//准备就绪处理进程间通信请求了。
	BINDER_LOOPER_STATE_REGISTERED  = 0x01,
	//如果一个线程是Binder驱动程序请求创建的，那么他就通过BC_REGISTER_LOOPER协议来通知Binder驱动程序，
	//这时Binder驱动程序就会增加他所请求的进程创建的Binder线程的数目。
	BINDER_LOOPER_STATE_ENTERED     = 0x02,
	//当一个Binder线程退出时，他会通过BC_EXIT_LOOPER协议来通知Binder驱动程序，这时Binder驱动程序就会
	//把他的状态设置为BINDER_LOOPER_STATE_EXITED。
	BINDER_LOOPER_STATE_EXITED      = 0x04,
	//异常情况下，一个Binder线程状态会被设置为BINDER_LOOPER_STATE_INVALID，例如，当线程已经处于
	//BINDER_LOOPER_STATE_REGISTERED状态时，如果他又再次通过BC_ENTER_LOOPER协议来通知Binder
	//驱动程序他已准备就绪了，那么Binder驱动程序就会将他的状态设置为BINDER_LOOPER_STATE_INVALID。
	BINDER_LOOPER_STATE_INVALID     = 0x08,
	//当一个Binder线程处于空闲状态时，Binder驱动程序就会把他的状态设置为BINDER_LOOPER_STATE_WAITING。
	BINDER_LOOPER_STATE_WAITING     = 0x10,
	//一个线程注册到Binder驱动程序时，Binder驱动程序就会为他创建一个binder_thread结构体，
	//并且将他的状态初始化为 BINDER_LOOPER_STATE_NEED_RETURN，表示该线程马上需要返回到
	//用户空间。由于一个线程在注册为Binder线程时可能还没准备好去处理进程间通信请求，因此，
	//最好返回到用户空间中去做准备工作。此外，当进程调用函数flush来刷新他的Binder线程池时，
	//Binder线程池中的线程的状态也会被重置为BINDER_LOOPERS_STATE_NEED_RETURN。
	BINDER_LOOPER_STATE_NEED_RETURN = 0x20
};

/**
 * @brief 描述Binder线程池中的一个线程
 * 
 */
struct binder_thread {
	//指向宿主进程
	struct binder_proc *proc;
	//前面在介绍进程结构体binder_proc时提到，进程结构体binder_proc使用一个红黑树来组织其Binder线程池中的线程，
	//其中，结构体binder_thread的成员变量rb_node就是该红黑树中的一个节点
	struct rb_node rb_node;
	//pid用于描述一个Binder线程的ID
	int pid;
	//looper用于描述一个Binder线程的状态。详细定义见上面<Binder线程状态取值>
	int looper;
	
	//当一个来自Client进程的请求指定要由某一个Binder线程来处理时，这个请求就会加入到相应的binder_thread结构体
	//成员变量todo所表示的队列中，并且唤醒这个线程来处理，因为这个线程可能处于睡眠状态。
	struct list_head todo;

	//当Binder驱动程序决定将一个事务交给一个Binder线程处理时，他就会将该事务封装成为一个binder_transaction
	//结构体，并且将它添加到由线程结构体binder_thread的成员变量transaction_stack所描述的一个事务堆栈中。
	//结构体binder_transaction设计很巧妙，后面我们再详细介绍他的定义
	struct binder_transaction *transaction_stack;
	//一个Binder线程在处理一个事务时，如果出现了异常情况，那么Binder驱动程序就会将相应的错误码保存在其成员变量
	//return_error和return_error2中，这时候线程就会将这些错误返回给用户空间应用程序处理。
	uint32_t return_error; /* Write failed, return error code in read buf */
	uint32_t return_error2; /* Write failed, return error code in read */
		/* buffer. Used when sending a reply to a dead process that */
		/* we are also waiting on */
	//当一个Binder线程在处理一个事务T1并需要依赖于其他的Binder线程来处理另外一个事务T2时，他就会睡眠在由成员变量
	//wait所描述的一个等待队列中，知道事务T2处理完成为止。
	wait_queue_head_t wait;
	//用来统计Binder线程数据，例如，Binder线程接收到的进程间通信请求的次数。
	struct binder_stats stats;
};

/**
 * @brief 用来描述进程间通信，这个过程又称为一个事务
 * 
 */
struct binder_transaction {
	int debug_id;

	//用来区分一个事务是同步的还是异步的。同步事务需要等待对方回复，这时候他的成员变量need_reply就是 1，否则就是0，
	//表示这是一个异步事务不需要等待回复
	unsigned need_reply : 1;

	//指事务的发起线程，称为源线程
	struct binder_thread *from;
	//通过priority和sender_euid，目标进程就可以识别事务发起方的身份
	//源线程的优先级
	long	priority;
	//源线程的用户ID
	uid_t	sender_euid;

	//负责处理该事务的进程，即目标进程
	struct binder_proc *to_proc;
	//负责处理该事务的线程，即目标线程
	struct binder_thread *to_thread;

	//一个线程在处理一个事务时，Binder驱动程序需要修改他的线程优先级，以便满足源线程和目标Service组件的要求。
	//Binder驱动程序在修改一个线程的优先级之前，会将它原来的优先级保存在一个事务结构体的成员变量saved_priority中，
	//以便线程处理完成该事务后可以恢复到原来的优先级。前面在介绍结构体binder_node时提到，目标线程在处理一个事务时，
	//他的线程优先级不能低于目标Service组件所要求的线程优先级，而且也不能低于源线程的优先级。这时候Binder驱动程序
	//就会将这二者中较大值设置为目标线程的优先级
	long	saved_priority;

	//指向Binder驱动程序为该事务分配的一块内核缓冲区，它里面保存了进程间通信数据。
	struct binder_buffer *buffer;

	//code和flags是直接从进程间通信数据中复制过来的，后面在介绍结构体binder_transaction_data时，再详细分析
	unsigned int	code;
	unsigned int	flags;

	//当Binder驱动程序为目标进程或者目标线程创建一个事务时，就会将该事务的成员变量work的值设置为
	//BINDER_WORK_TRANSACTION，并且将它添加到目标进程或者目标线程的todo队列中去等待处理。
	struct binder_work work;
	
	//假设线程A发起了一个事务T1，需要由线程B来处理；线程B在处理事务T1时，又需要线程C先处理事务T2；
	//线程C在处理T2时，又需要线程A先处理事务T3。这样事务T1就依赖于事务T2，而事务T2又依赖于事务T3。
	//他们关系如下：
	//			T2->from_parent=T1;
	//			T3->from_parent=T2;
	//对于线程A来说，他需要处理的事务有两个，分别是T1和T3，他首先要处理T3，然后才能处理事务T1，因此
	//事务T1和T3的关系如下：
	//			T3->to_parent=T1;
	//
	//考虑这样一个场景：如果线程C在发起事务T3给线程A所属的进程来处理时，Binder驱动程序选择了该进程的
	//另外一个线程D来处理该事务，这时候会出现什么情况呢？这时候线程A就会处于空闲等待状态，什么也不能做，
	//因为他必须要等待线程D处理完成事务T3后，才可以继续执行事务T1。在这种情况下，与其让线程A闲着，不如把事务
	//T3交给他来处理，这样线程D就可以去执行其他事务，提高了进程的并发性。
	//
	//那么问题来了，Binder驱动程序在分发事务T3给目标进程处理时，他是如何知道线程A属于目标进程，并且正在等待
	//事务T3的处理结果的？当线程C处理事务T2时，就会将事务T2放在其事务堆栈transaction_stack的最前端。
	//这样当线程C发起事务给T3给线程A所属的进程处理时，Binder驱动程序就可以沿着线程C的事务堆栈transaction_stack
	//向下遍历，即沿着事务T2的成员变量from_parent向下遍历，最后就会发现事务T3的目标进程等于事务T1的源进程，
	//并且事务T1是由线程A发起的，这时候他就知道线程A正在等待事务T3的处理结果了。

	//注意：线程A在处理事务T3时，Binder驱动程序会将事务T3放在其事务堆栈transaction_stack的最前端，
	//而在此之前，该事务堆栈transaction_stack的最前端指向的是事务T1。为了能够让线程A处理完事务T3后，
	//接着处理事务T1，Binder驱动程序就会将事务T1保存在事务T3的成员变量to_parent中。等到线程A处理完成
	//事务T3之后，就可以通过事务T3的成员变量找到事务T1，再将它放到线程A的事务堆栈transaction_stack的
	//最前端了。

	//一个事务所依赖的另外一个事务
	struct binder_transaction *from_parent;
	//目标线程的下一个需要处理的事务
	struct binder_transaction *to_parent;
	
	/*unsigned is_dead : 1;*/ /* not used at the moment */
};

static void binder_defer_work(struct binder_proc *proc, int defer);

/*
 * copied from get_unused_fd_flags
 */
int task_get_unused_fd_flags(struct binder_proc *proc, int flags)
{
	struct files_struct *files = proc->files;
	int fd, error;
	struct fdtable *fdt;
	unsigned long rlim_cur;
	unsigned long irqs;

	if (files == NULL)
		return -ESRCH;

	error = -EMFILE;
	spin_lock(&files->file_lock);

repeat:
	fdt = files_fdtable(files);
	fd = find_next_zero_bit(fdt->open_fds->fds_bits, fdt->max_fds,
				files->next_fd);

	/*
	 * N.B. For clone tasks sharing a files structure, this test
	 * will limit the total number of files that can be opened.
	 */
	rlim_cur = 0;
	if (lock_task_sighand(proc->tsk, &irqs)) {
		rlim_cur = proc->tsk->signal->rlim[RLIMIT_NOFILE].rlim_cur;
		unlock_task_sighand(proc->tsk, &irqs);
	}
	if (fd >= rlim_cur)
		goto out;

	/* Do we need to expand the fd array or fd set?  */
	error = expand_files(files, fd);
	if (error < 0)
		goto out;

	if (error) {
		/*
		 * If we needed to expand the fs array we
		 * might have blocked - try again.
		 */
		error = -EMFILE;
		goto repeat;
	}

	FD_SET(fd, fdt->open_fds);
	if (flags & O_CLOEXEC)
		FD_SET(fd, fdt->close_on_exec);
	else
		FD_CLR(fd, fdt->close_on_exec);
	files->next_fd = fd + 1;
#if 1
	/* Sanity check */
	if (fdt->fd[fd] != NULL) {
		printk(KERN_WARNING "get_unused_fd: slot %d not NULL!\n", fd);
		fdt->fd[fd] = NULL;
	}
#endif
	error = fd;

out:
	spin_unlock(&files->file_lock);
	return error;
}

/*
 * copied from fd_install
 */
static void task_fd_install(
	struct binder_proc *proc, unsigned int fd, struct file *file)
{
	struct files_struct *files = proc->files;
	struct fdtable *fdt;

	if (files == NULL)
		return;

	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	BUG_ON(fdt->fd[fd] != NULL);
	rcu_assign_pointer(fdt->fd[fd], file);
	spin_unlock(&files->file_lock);
}

/*
 * copied from __put_unused_fd in open.c
 */
static void __put_unused_fd(struct files_struct *files, unsigned int fd)
{
	struct fdtable *fdt = files_fdtable(files);
	__FD_CLR(fd, fdt->open_fds);
	if (fd < files->next_fd)
		files->next_fd = fd;
}

/*
 * copied from sys_close
 */
static long task_close_fd(struct binder_proc *proc, unsigned int fd)
{
	struct file *filp;
	struct files_struct *files = proc->files;
	struct fdtable *fdt;
	int retval;

	if (files == NULL)
		return -ESRCH;

	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	if (fd >= fdt->max_fds)
		goto out_unlock;
	filp = fdt->fd[fd];
	if (!filp)
		goto out_unlock;
	rcu_assign_pointer(fdt->fd[fd], NULL);
	FD_CLR(fd, fdt->close_on_exec);
	__put_unused_fd(files, fd);
	spin_unlock(&files->file_lock);
	retval = filp_close(filp, files);

	/* can't restart close syscall because file table entry was cleared */
	if (unlikely(retval == -ERESTARTSYS ||
		     retval == -ERESTARTNOINTR ||
		     retval == -ERESTARTNOHAND ||
		     retval == -ERESTART_RESTARTBLOCK))
		retval = -EINTR;

	return retval;

out_unlock:
	spin_unlock(&files->file_lock);
	return -EBADF;
}

static void binder_set_nice(long nice)
{
	long min_nice;
	if (can_nice(current, nice)) {
		set_user_nice(current, nice);
		return;
	}
	min_nice = 20 - current->signal->rlim[RLIMIT_NICE].rlim_cur;
	if (binder_debug_mask & BINDER_DEBUG_PRIORITY_CAP)
		printk(KERN_INFO "binder: %d: nice value %ld not allowed use "
		       "%ld instead\n", current->pid, nice, min_nice);
	set_user_nice(current, min_nice);
	if (min_nice < 20)
		return;
	binder_user_error("binder: %d RLIMIT_NICE not set\n", current->pid);
}

static size_t binder_buffer_size(
	struct binder_proc *proc, struct binder_buffer *buffer)
{
	if (list_is_last(&buffer->entry, &proc->buffers))
		return proc->buffer + proc->buffer_size - (void *)buffer->data;
	else
		return (size_t)list_entry(buffer->entry.next,
			struct binder_buffer, entry) - (size_t)buffer->data;
}

static void binder_insert_free_buffer(
	struct binder_proc *proc, struct binder_buffer *new_buffer)
{
	struct rb_node **p = &proc->free_buffers.rb_node;
	struct rb_node *parent = NULL;
	struct binder_buffer *buffer;
	size_t buffer_size;
	size_t new_buffer_size;

	BUG_ON(!new_buffer->free);

	new_buffer_size = binder_buffer_size(proc, new_buffer);

	if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC)
		printk(KERN_INFO "binder: %d: add free buffer, size %zd, "
		       "at %p\n", proc->pid, new_buffer_size, new_buffer);

	while (*p) {
		parent = *p;
		buffer = rb_entry(parent, struct binder_buffer, rb_node);
		BUG_ON(!buffer->free);

		buffer_size = binder_buffer_size(proc, buffer);

		if (new_buffer_size < buffer_size)
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}
	rb_link_node(&new_buffer->rb_node, parent, p);
	rb_insert_color(&new_buffer->rb_node, &proc->free_buffers);
}

static void binder_insert_allocated_buffer(
	struct binder_proc *proc, struct binder_buffer *new_buffer)
{
	struct rb_node **p = &proc->allocated_buffers.rb_node;
	struct rb_node *parent = NULL;
	struct binder_buffer *buffer;

	BUG_ON(new_buffer->free);

	while (*p) {
		parent = *p;
		buffer = rb_entry(parent, struct binder_buffer, rb_node);
		BUG_ON(buffer->free);

		if (new_buffer < buffer)
			p = &parent->rb_left;
		else if (new_buffer > buffer)
			p = &parent->rb_right;
		else
			BUG();
	}
	rb_link_node(&new_buffer->rb_node, parent, p);
	rb_insert_color(&new_buffer->rb_node, &proc->allocated_buffers);
}

static struct binder_buffer *binder_buffer_lookup(
	struct binder_proc *proc, void __user *user_ptr)
{
	struct rb_node *n = proc->allocated_buffers.rb_node;
	struct binder_buffer *buffer;
	struct binder_buffer *kern_ptr;

	kern_ptr = user_ptr - proc->user_buffer_offset
		- offsetof(struct binder_buffer, data);

	while (n) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);
		BUG_ON(buffer->free);

		if (kern_ptr < buffer)
			n = n->rb_left;
		else if (kern_ptr > buffer)
			n = n->rb_right;
		else
			return buffer;
	}
	return NULL;
}

static int binder_update_page_range(struct binder_proc *proc, int allocate,
	void *start, void *end, struct vm_area_struct *vma)
{
	void *page_addr;
	unsigned long user_page_addr;
	struct vm_struct tmp_area;
	struct page **page;
	struct mm_struct *mm;

	if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC)
		printk(KERN_INFO "binder: %d: %s pages %p-%p\n",
		       proc->pid, allocate ? "allocate" : "free", start, end);

	if (end <= start)
		return 0;

	if (vma)
		mm = NULL;
	else
		mm = get_task_mm(proc->tsk);

	if (mm) {
		down_write(&mm->mmap_sem);
		vma = proc->vma;
	}

	if (allocate == 0)
		goto free_range;

	if (vma == NULL) {
		printk(KERN_ERR "binder: %d: binder_alloc_buf failed to "
		       "map pages in userspace, no vma\n", proc->pid);
		goto err_no_vma;
	}

	for (page_addr = start; page_addr < end; page_addr += PAGE_SIZE) {
		int ret;
		struct page **page_array_ptr;
		page = &proc->pages[(page_addr - proc->buffer) / PAGE_SIZE];

		BUG_ON(*page);
		*page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (*page == NULL) {
			printk(KERN_ERR "binder: %d: binder_alloc_buf failed "
			       "for page at %p\n", proc->pid, page_addr);
			goto err_alloc_page_failed;
		}
		tmp_area.addr = page_addr;
		tmp_area.size = PAGE_SIZE + PAGE_SIZE /* guard page? */;
		page_array_ptr = page;
		ret = map_vm_area(&tmp_area, PAGE_KERNEL, &page_array_ptr);
		if (ret) {
			printk(KERN_ERR "binder: %d: binder_alloc_buf failed "
			       "to map page at %p in kernel\n",
			       proc->pid, page_addr);
			goto err_map_kernel_failed;
		}
		user_page_addr =
			(uintptr_t)page_addr + proc->user_buffer_offset;
		ret = vm_insert_page(vma, user_page_addr, page[0]);
		if (ret) {
			printk(KERN_ERR "binder: %d: binder_alloc_buf failed "
			       "to map page at %lx in userspace\n",
			       proc->pid, user_page_addr);
			goto err_vm_insert_page_failed;
		}
		/* vm_insert_page does not seem to increment the refcount */
	}
	if (mm) {
		up_write(&mm->mmap_sem);
		mmput(mm);
	}
	return 0;

free_range:
	for (page_addr = end - PAGE_SIZE; page_addr >= start;
	     page_addr -= PAGE_SIZE) {
		page = &proc->pages[(page_addr - proc->buffer) / PAGE_SIZE];
		if (vma)
			zap_page_range(vma, (uintptr_t)page_addr +
				proc->user_buffer_offset, PAGE_SIZE, NULL);
err_vm_insert_page_failed:
		unmap_kernel_range((unsigned long)page_addr, PAGE_SIZE);
err_map_kernel_failed:
		__free_page(*page);
		*page = NULL;
err_alloc_page_failed:
		;
	}
err_no_vma:
	if (mm) {
		up_write(&mm->mmap_sem);
		mmput(mm);
	}
	return -ENOMEM;
}

static struct binder_buffer *binder_alloc_buf(struct binder_proc *proc,
	size_t data_size, size_t offsets_size, int is_async)
{
	struct rb_node *n = proc->free_buffers.rb_node;
	struct binder_buffer *buffer;
	size_t buffer_size;
	struct rb_node *best_fit = NULL;
	void *has_page_addr;
	void *end_page_addr;
	size_t size;

	if (proc->vma == NULL) {
		printk(KERN_ERR "binder: %d: binder_alloc_buf, no vma\n",
		       proc->pid);
		return NULL;
	}

	size = ALIGN(data_size, sizeof(void *)) +
		ALIGN(offsets_size, sizeof(void *));

	if (size < data_size || size < offsets_size) {
		binder_user_error("binder: %d: got transaction with invalid "
			"size %zd-%zd\n", proc->pid, data_size, offsets_size);
		return NULL;
	}

	if (is_async &&
	    proc->free_async_space < size + sizeof(struct binder_buffer)) {
		if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC)
			printk(KERN_ERR "binder: %d: binder_alloc_buf size %zd f"
			       "ailed, no async space left\n", proc->pid, size);
		return NULL;
	}

	while (n) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);
		BUG_ON(!buffer->free);
		buffer_size = binder_buffer_size(proc, buffer);

		if (size < buffer_size) {
			best_fit = n;
			n = n->rb_left;
		} else if (size > buffer_size)
			n = n->rb_right;
		else {
			best_fit = n;
			break;
		}
	}
	if (best_fit == NULL) {
		printk(KERN_ERR "binder: %d: binder_alloc_buf size %zd failed, "
		       "no address space\n", proc->pid, size);
		return NULL;
	}
	if (n == NULL) {
		buffer = rb_entry(best_fit, struct binder_buffer, rb_node);
		buffer_size = binder_buffer_size(proc, buffer);
	}
	if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC)
		printk(KERN_INFO "binder: %d: binder_alloc_buf size %zd got buff"
		       "er %p size %zd\n", proc->pid, size, buffer, buffer_size);

	has_page_addr =
		(void *)(((uintptr_t)buffer->data + buffer_size) & PAGE_MASK);
	if (n == NULL) {
		if (size + sizeof(struct binder_buffer) + 4 >= buffer_size)
			buffer_size = size; /* no room for other buffers */
		else
			buffer_size = size + sizeof(struct binder_buffer);
	}
	end_page_addr =
		(void *)PAGE_ALIGN((uintptr_t)buffer->data + buffer_size);
	if (end_page_addr > has_page_addr)
		end_page_addr = has_page_addr;
	if (binder_update_page_range(proc, 1,
	    (void *)PAGE_ALIGN((uintptr_t)buffer->data), end_page_addr, NULL))
		return NULL;

	rb_erase(best_fit, &proc->free_buffers);
	buffer->free = 0;
	binder_insert_allocated_buffer(proc, buffer);
	if (buffer_size != size) {
		struct binder_buffer *new_buffer = (void *)buffer->data + size;
		list_add(&new_buffer->entry, &buffer->entry);
		new_buffer->free = 1;
		binder_insert_free_buffer(proc, new_buffer);
	}
	if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC)
		printk(KERN_INFO "binder: %d: binder_alloc_buf size %zd got "
		       "%p\n", proc->pid, size, buffer);
	buffer->data_size = data_size;
	buffer->offsets_size = offsets_size;
	buffer->async_transaction = is_async;
	if (is_async) {
		proc->free_async_space -= size + sizeof(struct binder_buffer);
		if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC_ASYNC)
			printk(KERN_INFO "binder: %d: binder_alloc_buf size %zd "
			       "async free %zd\n", proc->pid, size,
			       proc->free_async_space);
	}

	return buffer;
}

static void *buffer_start_page(struct binder_buffer *buffer)
{
	return (void *)((uintptr_t)buffer & PAGE_MASK);
}

static void *buffer_end_page(struct binder_buffer *buffer)
{
	return (void *)(((uintptr_t)(buffer + 1) - 1) & PAGE_MASK);
}

static void binder_delete_free_buffer(
	struct binder_proc *proc, struct binder_buffer *buffer)
{
	struct binder_buffer *prev, *next = NULL;
	int free_page_end = 1;
	int free_page_start = 1;

	BUG_ON(proc->buffers.next == &buffer->entry);
	prev = list_entry(buffer->entry.prev, struct binder_buffer, entry);
	BUG_ON(!prev->free);
	if (buffer_end_page(prev) == buffer_start_page(buffer)) {
		free_page_start = 0;
		if (buffer_end_page(prev) == buffer_end_page(buffer))
			free_page_end = 0;
		if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC)
			printk(KERN_INFO "binder: %d: merge free, buffer %p "
			       "share page with %p\n", proc->pid, buffer, prev);
	}

	if (!list_is_last(&buffer->entry, &proc->buffers)) {
		next = list_entry(buffer->entry.next,
				  struct binder_buffer, entry);
		if (buffer_start_page(next) == buffer_end_page(buffer)) {
			free_page_end = 0;
			if (buffer_start_page(next) ==
			    buffer_start_page(buffer))
				free_page_start = 0;
			if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC)
				printk(KERN_INFO "binder: %d: merge free, "
				       "buffer %p share page with %p\n",
				       proc->pid, buffer, prev);
		}
	}
	list_del(&buffer->entry);
	if (free_page_start || free_page_end) {
		if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC)
			printk(KERN_INFO "binder: %d: merge free, buffer %p do "
			       "not share page%s%s with with %p or %p\n",
			       proc->pid, buffer, free_page_start ? "" : " end",
			       free_page_end ? "" : " start", prev, next);
		binder_update_page_range(proc, 0, free_page_start ?
			buffer_start_page(buffer) : buffer_end_page(buffer),
			(free_page_end ? buffer_end_page(buffer) :
			buffer_start_page(buffer)) + PAGE_SIZE, NULL);
	}
}

static void binder_free_buf(
	struct binder_proc *proc, struct binder_buffer *buffer)
{
	size_t size, buffer_size;

	buffer_size = binder_buffer_size(proc, buffer);

	size = ALIGN(buffer->data_size, sizeof(void *)) +
		ALIGN(buffer->offsets_size, sizeof(void *));
	if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC)
		printk(KERN_INFO "binder: %d: binder_free_buf %p size %zd buffer"
		       "_size %zd\n", proc->pid, buffer, size, buffer_size);

	BUG_ON(buffer->free);
	BUG_ON(size > buffer_size);
	BUG_ON(buffer->transaction != NULL);
	BUG_ON((void *)buffer < proc->buffer);
	BUG_ON((void *)buffer > proc->buffer + proc->buffer_size);

	if (buffer->async_transaction) {
		proc->free_async_space += size + sizeof(struct binder_buffer);
		if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC_ASYNC)
			printk(KERN_INFO "binder: %d: binder_free_buf size %zd "
			       "async free %zd\n", proc->pid, size,
			       proc->free_async_space);
	}

	binder_update_page_range(proc, 0,
		(void *)PAGE_ALIGN((uintptr_t)buffer->data),
		(void *)(((uintptr_t)buffer->data + buffer_size) & PAGE_MASK),
		NULL);
	rb_erase(&buffer->rb_node, &proc->allocated_buffers);
	buffer->free = 1;
	if (!list_is_last(&buffer->entry, &proc->buffers)) {
		struct binder_buffer *next = list_entry(buffer->entry.next,
						struct binder_buffer, entry);
		if (next->free) {
			rb_erase(&next->rb_node, &proc->free_buffers);
			binder_delete_free_buffer(proc, next);
		}
	}
	if (proc->buffers.next != &buffer->entry) {
		struct binder_buffer *prev = list_entry(buffer->entry.prev,
						struct binder_buffer, entry);
		if (prev->free) {
			binder_delete_free_buffer(proc, buffer);
			rb_erase(&prev->rb_node, &proc->free_buffers);
			buffer = prev;
		}
	}
	binder_insert_free_buffer(proc, buffer);
}

static struct binder_node *
binder_get_node(struct binder_proc *proc, void __user *ptr)
{
	struct rb_node *n = proc->nodes.rb_node;
	struct binder_node *node;

	while (n) {
		node = rb_entry(n, struct binder_node, rb_node);

		if (ptr < node->ptr)
			n = n->rb_left;
		else if (ptr > node->ptr)
			n = n->rb_right;
		else
			return node;
	}
	return NULL;
}

static struct binder_node *
binder_new_node(struct binder_proc *proc, void __user *ptr, void __user *cookie)
{
	struct rb_node **p = &proc->nodes.rb_node;
	struct rb_node *parent = NULL;
	struct binder_node *node;

	while (*p) {
		parent = *p;
		node = rb_entry(parent, struct binder_node, rb_node);

		if (ptr < node->ptr)
			p = &(*p)->rb_left;
		else if (ptr > node->ptr)
			p = &(*p)->rb_right;
		else
			return NULL;
	}

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (node == NULL)
		return NULL;
	binder_stats.obj_created[BINDER_STAT_NODE]++;
	rb_link_node(&node->rb_node, parent, p);
	rb_insert_color(&node->rb_node, &proc->nodes);
	node->debug_id = ++binder_last_id;
	node->proc = proc;
	node->ptr = ptr;
	node->cookie = cookie;
	node->work.type = BINDER_WORK_NODE;
	INIT_LIST_HEAD(&node->work.entry);
	INIT_LIST_HEAD(&node->async_todo);
	if (binder_debug_mask & BINDER_DEBUG_INTERNAL_REFS)
		printk(KERN_INFO "binder: %d:%d node %d u%p c%p created\n",
		       proc->pid, current->pid, node->debug_id,
		       node->ptr, node->cookie);
	return node;
}

static int
binder_inc_node(struct binder_node *node, int strong, int internal,
		struct list_head *target_list)
{
	if (strong) {
		if (internal) {
			if (target_list == NULL &&
			    node->internal_strong_refs == 0 &&
			    !(node == binder_context_mgr_node &&
			    node->has_strong_ref)) {
				printk(KERN_ERR "binder: invalid inc strong "
					"node for %d\n", node->debug_id);
				return -EINVAL;
			}
			node->internal_strong_refs++;
		} else
			node->local_strong_refs++;
		if (!node->has_strong_ref && target_list) {
			list_del_init(&node->work.entry);
			list_add_tail(&node->work.entry, target_list);
		}
	} else {
		if (!internal)
			node->local_weak_refs++;
		if (!node->has_weak_ref && list_empty(&node->work.entry)) {
			if (target_list == NULL) {
				printk(KERN_ERR "binder: invalid inc weak node "
					"for %d\n", node->debug_id);
				return -EINVAL;
			}
			list_add_tail(&node->work.entry, target_list);
		}
	}
	return 0;
}

static int
binder_dec_node(struct binder_node *node, int strong, int internal)
{
	if (strong) {
		if (internal)
			node->internal_strong_refs--;
		else
			node->local_strong_refs--;
		if (node->local_strong_refs || node->internal_strong_refs)
			return 0;
	} else {
		if (!internal)
			node->local_weak_refs--;
		if (node->local_weak_refs || !hlist_empty(&node->refs))
			return 0;
	}
	if (node->proc && (node->has_strong_ref || node->has_weak_ref)) {
		if (list_empty(&node->work.entry)) {
			list_add_tail(&node->work.entry, &node->proc->todo);
			wake_up_interruptible(&node->proc->wait);
		}
	} else {
		if (hlist_empty(&node->refs) && !node->local_strong_refs &&
		    !node->local_weak_refs) {
			list_del_init(&node->work.entry);
			if (node->proc) {
				rb_erase(&node->rb_node, &node->proc->nodes);
				if (binder_debug_mask & BINDER_DEBUG_INTERNAL_REFS)
					printk(KERN_INFO "binder: refless node %d deleted\n", node->debug_id);
			} else {
				hlist_del(&node->dead_node);
				if (binder_debug_mask & BINDER_DEBUG_INTERNAL_REFS)
					printk(KERN_INFO "binder: dead node %d deleted\n", node->debug_id);
			}
			kfree(node);
			binder_stats.obj_deleted[BINDER_STAT_NODE]++;
		}
	}

	return 0;
}


static struct binder_ref *
binder_get_ref(struct binder_proc *proc, uint32_t desc)
{
	struct rb_node *n = proc->refs_by_desc.rb_node;
	struct binder_ref *ref;

	while (n) {
		ref = rb_entry(n, struct binder_ref, rb_node_desc);

		if (desc < ref->desc)
			n = n->rb_left;
		else if (desc > ref->desc)
			n = n->rb_right;
		else
			return ref;
	}
	return NULL;
}

static struct binder_ref *
binder_get_ref_for_node(struct binder_proc *proc, struct binder_node *node)
{
	struct rb_node *n;
	struct rb_node **p = &proc->refs_by_node.rb_node;
	struct rb_node *parent = NULL;
	struct binder_ref *ref, *new_ref;

	while (*p) {
		parent = *p;
		ref = rb_entry(parent, struct binder_ref, rb_node_node);

		if (node < ref->node)
			p = &(*p)->rb_left;
		else if (node > ref->node)
			p = &(*p)->rb_right;
		else
			return ref;
	}
	new_ref = kzalloc(sizeof(*ref), GFP_KERNEL);
	if (new_ref == NULL)
		return NULL;
	binder_stats.obj_created[BINDER_STAT_REF]++;
	new_ref->debug_id = ++binder_last_id;
	new_ref->proc = proc;
	new_ref->node = node;
	rb_link_node(&new_ref->rb_node_node, parent, p);
	rb_insert_color(&new_ref->rb_node_node, &proc->refs_by_node);

	new_ref->desc = (node == binder_context_mgr_node) ? 0 : 1;
	for (n = rb_first(&proc->refs_by_desc); n != NULL; n = rb_next(n)) {
		ref = rb_entry(n, struct binder_ref, rb_node_desc);
		if (ref->desc > new_ref->desc)
			break;
		new_ref->desc = ref->desc + 1;
	}

	p = &proc->refs_by_desc.rb_node;
	while (*p) {
		parent = *p;
		ref = rb_entry(parent, struct binder_ref, rb_node_desc);

		if (new_ref->desc < ref->desc)
			p = &(*p)->rb_left;
		else if (new_ref->desc > ref->desc)
			p = &(*p)->rb_right;
		else
			BUG();
	}
	rb_link_node(&new_ref->rb_node_desc, parent, p);
	rb_insert_color(&new_ref->rb_node_desc, &proc->refs_by_desc);
	if (node) {
		hlist_add_head(&new_ref->node_entry, &node->refs);
		if (binder_debug_mask & BINDER_DEBUG_INTERNAL_REFS)
			printk(KERN_INFO "binder: %d new ref %d desc %d for "
				"node %d\n", proc->pid, new_ref->debug_id,
				new_ref->desc, node->debug_id);
	} else {
		if (binder_debug_mask & BINDER_DEBUG_INTERNAL_REFS)
			printk(KERN_INFO "binder: %d new ref %d desc %d for "
				"dead node\n", proc->pid, new_ref->debug_id,
				new_ref->desc);
	}
	return new_ref;
}

static void
binder_delete_ref(struct binder_ref *ref)
{
	if (binder_debug_mask & BINDER_DEBUG_INTERNAL_REFS)
		printk(KERN_INFO "binder: %d delete ref %d desc %d for "
			"node %d\n", ref->proc->pid, ref->debug_id,
			ref->desc, ref->node->debug_id);
	rb_erase(&ref->rb_node_desc, &ref->proc->refs_by_desc);
	rb_erase(&ref->rb_node_node, &ref->proc->refs_by_node);
	if (ref->strong)
		binder_dec_node(ref->node, 1, 1);
	hlist_del(&ref->node_entry);
	binder_dec_node(ref->node, 0, 1);
	if (ref->death) {
		if (binder_debug_mask & BINDER_DEBUG_DEAD_BINDER)
			printk(KERN_INFO "binder: %d delete ref %d desc %d "
				"has death notification\n", ref->proc->pid,
				ref->debug_id, ref->desc);
		list_del(&ref->death->work.entry);
		kfree(ref->death);
		binder_stats.obj_deleted[BINDER_STAT_DEATH]++;
	}
	kfree(ref);
	binder_stats.obj_deleted[BINDER_STAT_REF]++;
}

static int
binder_inc_ref(
	struct binder_ref *ref, int strong, struct list_head *target_list)
{
	int ret;
	if (strong) {
		if (ref->strong == 0) {
			ret = binder_inc_node(ref->node, 1, 1, target_list);
			if (ret)
				return ret;
		}
		ref->strong++;
	} else {
		if (ref->weak == 0) {
			ret = binder_inc_node(ref->node, 0, 1, target_list);
			if (ret)
				return ret;
		}
		ref->weak++;
	}
	return 0;
}


static int
binder_dec_ref(struct binder_ref *ref, int strong)
{
	if (strong) {
		if (ref->strong == 0) {
			binder_user_error("binder: %d invalid dec strong, "
					  "ref %d desc %d s %d w %d\n",
					  ref->proc->pid, ref->debug_id,
					  ref->desc, ref->strong, ref->weak);
			return -EINVAL;
		}
		ref->strong--;
		if (ref->strong == 0) {
			int ret;
			ret = binder_dec_node(ref->node, strong, 1);
			if (ret)
				return ret;
		}
	} else {
		if (ref->weak == 0) {
			binder_user_error("binder: %d invalid dec weak, "
					  "ref %d desc %d s %d w %d\n",
					  ref->proc->pid, ref->debug_id,
					  ref->desc, ref->strong, ref->weak);
			return -EINVAL;
		}
		ref->weak--;
	}
	if (ref->strong == 0 && ref->weak == 0)
		binder_delete_ref(ref);
	return 0;
}

static void
binder_pop_transaction(
	struct binder_thread *target_thread, struct binder_transaction *t)
{
	if (target_thread) {
		BUG_ON(target_thread->transaction_stack != t);
		BUG_ON(target_thread->transaction_stack->from != target_thread);
		target_thread->transaction_stack =
			target_thread->transaction_stack->from_parent;
		t->from = NULL;
	}
	t->need_reply = 0;
	if (t->buffer)
		t->buffer->transaction = NULL;
	kfree(t);
	binder_stats.obj_deleted[BINDER_STAT_TRANSACTION]++;
}

static void
binder_send_failed_reply(struct binder_transaction *t, uint32_t error_code)
{
	struct binder_thread *target_thread;
	BUG_ON(t->flags & TF_ONE_WAY);
	while (1) {
		target_thread = t->from;
		if (target_thread) {
			if (target_thread->return_error != BR_OK &&
			   target_thread->return_error2 == BR_OK) {
				target_thread->return_error2 =
					target_thread->return_error;
				target_thread->return_error = BR_OK;
			}
			if (target_thread->return_error == BR_OK) {
				if (binder_debug_mask & BINDER_DEBUG_FAILED_TRANSACTION)
					printk(KERN_INFO "binder: send failed reply for transaction %d to %d:%d\n",
					       t->debug_id, target_thread->proc->pid, target_thread->pid);

				binder_pop_transaction(target_thread, t);
				target_thread->return_error = error_code;
				wake_up_interruptible(&target_thread->wait);
			} else {
				printk(KERN_ERR "binder: reply failed, target "
					"thread, %d:%d, has error code %d "
					"already\n", target_thread->proc->pid,
					target_thread->pid,
					target_thread->return_error);
			}
			return;
		} else {
			struct binder_transaction *next = t->from_parent;

			if (binder_debug_mask & BINDER_DEBUG_FAILED_TRANSACTION)
				printk(KERN_INFO "binder: send failed reply "
					"for transaction %d, target dead\n",
					t->debug_id);

			binder_pop_transaction(target_thread, t);
			if (next == NULL) {
				if (binder_debug_mask & BINDER_DEBUG_DEAD_BINDER)
					printk(KERN_INFO "binder: reply failed,"
						" no target thread at root\n");
				return;
			}
			t = next;
			if (binder_debug_mask & BINDER_DEBUG_DEAD_BINDER)
				printk(KERN_INFO "binder: reply failed, no targ"
					"et thread -- retry %d\n", t->debug_id);
		}
	}
}

static void
binder_transaction_buffer_release(struct binder_proc *proc,
			struct binder_buffer *buffer, size_t *failed_at);

static void
binder_transaction(struct binder_proc *proc, struct binder_thread *thread,
	struct binder_transaction_data *tr, int reply)
{
	struct binder_transaction *t;
	struct binder_work *tcomplete;
	size_t *offp, *off_end;
	struct binder_proc *target_proc;
	struct binder_thread *target_thread = NULL;
	struct binder_node *target_node = NULL;
	struct list_head *target_list;
	wait_queue_head_t *target_wait;
	struct binder_transaction *in_reply_to = NULL;
	struct binder_transaction_log_entry *e;
	uint32_t return_error;

	e = binder_transaction_log_add(&binder_transaction_log);
	e->call_type = reply ? 2 : !!(tr->flags & TF_ONE_WAY);
	e->from_proc = proc->pid;
	e->from_thread = thread->pid;
	e->target_handle = tr->target.handle;
	e->data_size = tr->data_size;
	e->offsets_size = tr->offsets_size;

	if (reply) {
		in_reply_to = thread->transaction_stack;
		if (in_reply_to == NULL) {
			binder_user_error("binder: %d:%d got reply transaction "
					  "with no transaction stack\n",
					  proc->pid, thread->pid);
			return_error = BR_FAILED_REPLY;
			goto err_empty_call_stack;
		}
		binder_set_nice(in_reply_to->saved_priority);
		if (in_reply_to->to_thread != thread) {
			binder_user_error("binder: %d:%d got reply transaction "
				"with bad transaction stack,"
				" transaction %d has target %d:%d\n",
				proc->pid, thread->pid, in_reply_to->debug_id,
				in_reply_to->to_proc ?
				in_reply_to->to_proc->pid : 0,
				in_reply_to->to_thread ?
				in_reply_to->to_thread->pid : 0);
			return_error = BR_FAILED_REPLY;
			in_reply_to = NULL;
			goto err_bad_call_stack;
		}
		thread->transaction_stack = in_reply_to->to_parent;
		target_thread = in_reply_to->from;
		if (target_thread == NULL) {
			return_error = BR_DEAD_REPLY;
			goto err_dead_binder;
		}
		if (target_thread->transaction_stack != in_reply_to) {
			binder_user_error("binder: %d:%d got reply transaction "
				"with bad target transaction stack %d, "
				"expected %d\n",
				proc->pid, thread->pid,
				target_thread->transaction_stack ?
				target_thread->transaction_stack->debug_id : 0,
				in_reply_to->debug_id);
			return_error = BR_FAILED_REPLY;
			in_reply_to = NULL;
			target_thread = NULL;
			goto err_dead_binder;
		}
		target_proc = target_thread->proc;
	} else {
		if (tr->target.handle) {
			struct binder_ref *ref;
			ref = binder_get_ref(proc, tr->target.handle);
			if (ref == NULL) {
				binder_user_error("binder: %d:%d got "
					"transaction to invalid handle\n",
					proc->pid, thread->pid);
				return_error = BR_FAILED_REPLY;
				goto err_invalid_target_handle;
			}
			target_node = ref->node;
		} else {
			target_node = binder_context_mgr_node;
			if (target_node == NULL) {
				return_error = BR_DEAD_REPLY;
				goto err_no_context_mgr_node;
			}
		}
		e->to_node = target_node->debug_id;
		target_proc = target_node->proc;
		if (target_proc == NULL) {
			return_error = BR_DEAD_REPLY;
			goto err_dead_binder;
		}
		if (!(tr->flags & TF_ONE_WAY) && thread->transaction_stack) {
			struct binder_transaction *tmp;
			tmp = thread->transaction_stack;
			if (tmp->to_thread != thread) {
				binder_user_error("binder: %d:%d got new "
					"transaction with bad transaction stack"
					", transaction %d has target %d:%d\n",
					proc->pid, thread->pid, tmp->debug_id,
					tmp->to_proc ? tmp->to_proc->pid : 0,
					tmp->to_thread ?
					tmp->to_thread->pid : 0);
				return_error = BR_FAILED_REPLY;
				goto err_bad_call_stack;
			}
			while (tmp) {
				if (tmp->from && tmp->from->proc == target_proc)
					target_thread = tmp->from;
				tmp = tmp->from_parent;
			}
		}
	}
	if (target_thread) {
		e->to_thread = target_thread->pid;
		target_list = &target_thread->todo;
		target_wait = &target_thread->wait;
	} else {
		target_list = &target_proc->todo;
		target_wait = &target_proc->wait;
	}
	e->to_proc = target_proc->pid;

	/* TODO: reuse incoming transaction for reply */
	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (t == NULL) {
		return_error = BR_FAILED_REPLY;
		goto err_alloc_t_failed;
	}
	binder_stats.obj_created[BINDER_STAT_TRANSACTION]++;

	tcomplete = kzalloc(sizeof(*tcomplete), GFP_KERNEL);
	if (tcomplete == NULL) {
		return_error = BR_FAILED_REPLY;
		goto err_alloc_tcomplete_failed;
	}
	binder_stats.obj_created[BINDER_STAT_TRANSACTION_COMPLETE]++;

	t->debug_id = ++binder_last_id;
	e->debug_id = t->debug_id;

	if (binder_debug_mask & BINDER_DEBUG_TRANSACTION) {
		if (reply)
			printk(KERN_INFO "binder: %d:%d BC_REPLY %d -> %d:%d, "
			       "data %p-%p size %zd-%zd\n",
			       proc->pid, thread->pid, t->debug_id,
			       target_proc->pid, target_thread->pid,
			       tr->data.ptr.buffer, tr->data.ptr.offsets,
			       tr->data_size, tr->offsets_size);
		else
			printk(KERN_INFO "binder: %d:%d BC_TRANSACTION %d -> "
			       "%d - node %d, data %p-%p size %zd-%zd\n",
			       proc->pid, thread->pid, t->debug_id,
			       target_proc->pid, target_node->debug_id,
			       tr->data.ptr.buffer, tr->data.ptr.offsets,
			       tr->data_size, tr->offsets_size);
	}

	if (!reply && !(tr->flags & TF_ONE_WAY))
		t->from = thread;
	else
		t->from = NULL;
	t->sender_euid = proc->tsk->cred->euid;
	t->to_proc = target_proc;
	t->to_thread = target_thread;
	t->code = tr->code;
	t->flags = tr->flags;
	t->priority = task_nice(current);
	t->buffer = binder_alloc_buf(target_proc, tr->data_size,
		tr->offsets_size, !reply && (t->flags & TF_ONE_WAY));
	if (t->buffer == NULL) {
		return_error = BR_FAILED_REPLY;
		goto err_binder_alloc_buf_failed;
	}
	t->buffer->allow_user_free = 0;
	t->buffer->debug_id = t->debug_id;
	t->buffer->transaction = t;
	t->buffer->target_node = target_node;
	if (target_node)
		binder_inc_node(target_node, 1, 0, NULL);

	offp = (size_t *)(t->buffer->data + ALIGN(tr->data_size, sizeof(void *)));

	if (copy_from_user(t->buffer->data, tr->data.ptr.buffer, tr->data_size)) {
		binder_user_error("binder: %d:%d got transaction with invalid "
			"data ptr\n", proc->pid, thread->pid);
		return_error = BR_FAILED_REPLY;
		goto err_copy_data_failed;
	}
	if (copy_from_user(offp, tr->data.ptr.offsets, tr->offsets_size)) {
		binder_user_error("binder: %d:%d got transaction with invalid "
			"offsets ptr\n", proc->pid, thread->pid);
		return_error = BR_FAILED_REPLY;
		goto err_copy_data_failed;
	}
	if (!IS_ALIGNED(tr->offsets_size, sizeof(size_t))) {
		binder_user_error("binder: %d:%d got transaction with "
			"invalid offsets size, %zd\n",
			proc->pid, thread->pid, tr->offsets_size);
		return_error = BR_FAILED_REPLY;
		goto err_bad_offset;
	}
	off_end = (void *)offp + tr->offsets_size;
	for (; offp < off_end; offp++) {
		struct flat_binder_object *fp;
		if (*offp > t->buffer->data_size - sizeof(*fp) ||
		    t->buffer->data_size < sizeof(*fp) ||
		    !IS_ALIGNED(*offp, sizeof(void *))) {
			binder_user_error("binder: %d:%d got transaction with "
				"invalid offset, %zd\n",
				proc->pid, thread->pid, *offp);
			return_error = BR_FAILED_REPLY;
			goto err_bad_offset;
		}
		fp = (struct flat_binder_object *)(t->buffer->data + *offp);
		switch (fp->type) {
		case BINDER_TYPE_BINDER:
		case BINDER_TYPE_WEAK_BINDER: {
			struct binder_ref *ref;
			struct binder_node *node = binder_get_node(proc, fp->binder);
			if (node == NULL) {
				node = binder_new_node(proc, fp->binder, fp->cookie);
				if (node == NULL) {
					return_error = BR_FAILED_REPLY;
					goto err_binder_new_node_failed;
				}
				node->min_priority = fp->flags & FLAT_BINDER_FLAG_PRIORITY_MASK;
				node->accept_fds = !!(fp->flags & FLAT_BINDER_FLAG_ACCEPTS_FDS);
			}
			if (fp->cookie != node->cookie) {
				binder_user_error("binder: %d:%d sending u%p "
					"node %d, cookie mismatch %p != %p\n",
					proc->pid, thread->pid,
					fp->binder, node->debug_id,
					fp->cookie, node->cookie);
				goto err_binder_get_ref_for_node_failed;
			}
			ref = binder_get_ref_for_node(target_proc, node);
			if (ref == NULL) {
				return_error = BR_FAILED_REPLY;
				goto err_binder_get_ref_for_node_failed;
			}
			if (fp->type == BINDER_TYPE_BINDER)
				fp->type = BINDER_TYPE_HANDLE;
			else
				fp->type = BINDER_TYPE_WEAK_HANDLE;
			fp->handle = ref->desc;
			binder_inc_ref(ref, fp->type == BINDER_TYPE_HANDLE, &thread->todo);
			if (binder_debug_mask & BINDER_DEBUG_TRANSACTION)
				printk(KERN_INFO "        node %d u%p -> ref %d desc %d\n",
				       node->debug_id, node->ptr, ref->debug_id, ref->desc);
		} break;
		case BINDER_TYPE_HANDLE:
		case BINDER_TYPE_WEAK_HANDLE: {
			struct binder_ref *ref = binder_get_ref(proc, fp->handle);
			if (ref == NULL) {
				binder_user_error("binder: %d:%d got "
					"transaction with invalid "
					"handle, %ld\n", proc->pid,
					thread->pid, fp->handle);
				return_error = BR_FAILED_REPLY;
				goto err_binder_get_ref_failed;
			}
			if (ref->node->proc == target_proc) {
				if (fp->type == BINDER_TYPE_HANDLE)
					fp->type = BINDER_TYPE_BINDER;
				else
					fp->type = BINDER_TYPE_WEAK_BINDER;
				fp->binder = ref->node->ptr;
				fp->cookie = ref->node->cookie;
				binder_inc_node(ref->node, fp->type == BINDER_TYPE_BINDER, 0, NULL);
				if (binder_debug_mask & BINDER_DEBUG_TRANSACTION)
					printk(KERN_INFO "        ref %d desc %d -> node %d u%p\n",
					       ref->debug_id, ref->desc, ref->node->debug_id, ref->node->ptr);
			} else {
				struct binder_ref *new_ref;
				new_ref = binder_get_ref_for_node(target_proc, ref->node);
				if (new_ref == NULL) {
					return_error = BR_FAILED_REPLY;
					goto err_binder_get_ref_for_node_failed;
				}
				fp->handle = new_ref->desc;
				binder_inc_ref(new_ref, fp->type == BINDER_TYPE_HANDLE, NULL);
				if (binder_debug_mask & BINDER_DEBUG_TRANSACTION)
					printk(KERN_INFO "        ref %d desc %d -> ref %d desc %d (node %d)\n",
					       ref->debug_id, ref->desc, new_ref->debug_id, new_ref->desc, ref->node->debug_id);
			}
		} break;

		case BINDER_TYPE_FD: {
			int target_fd;
			struct file *file;

			if (reply) {
				if (!(in_reply_to->flags & TF_ACCEPT_FDS)) {
					binder_user_error("binder: %d:%d got reply with fd, %ld, but target does not allow fds\n",
						proc->pid, thread->pid, fp->handle);
					return_error = BR_FAILED_REPLY;
					goto err_fd_not_allowed;
				}
			} else if (!target_node->accept_fds) {
				binder_user_error("binder: %d:%d got transaction with fd, %ld, but target does not allow fds\n",
					proc->pid, thread->pid, fp->handle);
				return_error = BR_FAILED_REPLY;
				goto err_fd_not_allowed;
			}

			file = fget(fp->handle);
			if (file == NULL) {
				binder_user_error("binder: %d:%d got transaction with invalid fd, %ld\n",
					proc->pid, thread->pid, fp->handle);
				return_error = BR_FAILED_REPLY;
				goto err_fget_failed;
			}
			target_fd = task_get_unused_fd_flags(target_proc, O_CLOEXEC);
			if (target_fd < 0) {
				fput(file);
				return_error = BR_FAILED_REPLY;
				goto err_get_unused_fd_failed;
			}
			task_fd_install(target_proc, target_fd, file);
			if (binder_debug_mask & BINDER_DEBUG_TRANSACTION)
				printk(KERN_INFO "        fd %ld -> %d\n", fp->handle, target_fd);
			/* TODO: fput? */
			fp->handle = target_fd;
		} break;

		default:
			binder_user_error("binder: %d:%d got transactio"
				"n with invalid object type, %lx\n",
				proc->pid, thread->pid, fp->type);
			return_error = BR_FAILED_REPLY;
			goto err_bad_object_type;
		}
	}
	if (reply) {
		BUG_ON(t->buffer->async_transaction != 0);
		binder_pop_transaction(target_thread, in_reply_to);
	} else if (!(t->flags & TF_ONE_WAY)) {
		BUG_ON(t->buffer->async_transaction != 0);
		t->need_reply = 1;
		t->from_parent = thread->transaction_stack;
		thread->transaction_stack = t;
	} else {
		BUG_ON(target_node == NULL);
		BUG_ON(t->buffer->async_transaction != 1);
		if (target_node->has_async_transaction) {
			target_list = &target_node->async_todo;
			target_wait = NULL;
		} else
			target_node->has_async_transaction = 1;
	}
	t->work.type = BINDER_WORK_TRANSACTION;
	list_add_tail(&t->work.entry, target_list);
	tcomplete->type = BINDER_WORK_TRANSACTION_COMPLETE;
	list_add_tail(&tcomplete->entry, &thread->todo);
	if (target_wait)
		wake_up_interruptible(target_wait);
	return;

err_get_unused_fd_failed:
err_fget_failed:
err_fd_not_allowed:
err_binder_get_ref_for_node_failed:
err_binder_get_ref_failed:
err_binder_new_node_failed:
err_bad_object_type:
err_bad_offset:
err_copy_data_failed:
	binder_transaction_buffer_release(target_proc, t->buffer, offp);
	t->buffer->transaction = NULL;
	binder_free_buf(target_proc, t->buffer);
err_binder_alloc_buf_failed:
	kfree(tcomplete);
	binder_stats.obj_deleted[BINDER_STAT_TRANSACTION_COMPLETE]++;
err_alloc_tcomplete_failed:
	kfree(t);
	binder_stats.obj_deleted[BINDER_STAT_TRANSACTION]++;
err_alloc_t_failed:
err_bad_call_stack:
err_empty_call_stack:
err_dead_binder:
err_invalid_target_handle:
err_no_context_mgr_node:
	if (binder_debug_mask & BINDER_DEBUG_FAILED_TRANSACTION)
		printk(KERN_INFO "binder: %d:%d transaction failed %d, size"
				"%zd-%zd\n",
			   proc->pid, thread->pid, return_error,
			   tr->data_size, tr->offsets_size);

	{
		struct binder_transaction_log_entry *fe;
		fe = binder_transaction_log_add(&binder_transaction_log_failed);
		*fe = *e;
	}

	BUG_ON(thread->return_error != BR_OK);
	if (in_reply_to) {
		thread->return_error = BR_TRANSACTION_COMPLETE;
		binder_send_failed_reply(in_reply_to, return_error);
	} else
		thread->return_error = return_error;
}

static void
binder_transaction_buffer_release(struct binder_proc *proc, struct binder_buffer *buffer, size_t *failed_at)
{
	size_t *offp, *off_end;
	int debug_id = buffer->debug_id;

	if (binder_debug_mask & BINDER_DEBUG_TRANSACTION)
		printk(KERN_INFO "binder: %d buffer release %d, size %zd-%zd, failed at %p\n",
			   proc->pid, buffer->debug_id,
			   buffer->data_size, buffer->offsets_size, failed_at);

	if (buffer->target_node)
		binder_dec_node(buffer->target_node, 1, 0);

	offp = (size_t *)(buffer->data + ALIGN(buffer->data_size, sizeof(void *)));
	if (failed_at)
		off_end = failed_at;
	else
		off_end = (void *)offp + buffer->offsets_size;
	for (; offp < off_end; offp++) {
		struct flat_binder_object *fp;
		if (*offp > buffer->data_size - sizeof(*fp) ||
		    buffer->data_size < sizeof(*fp) ||
		    !IS_ALIGNED(*offp, sizeof(void *))) {
			printk(KERN_ERR "binder: transaction release %d bad"
					"offset %zd, size %zd\n", debug_id, *offp, buffer->data_size);
			continue;
		}
		fp = (struct flat_binder_object *)(buffer->data + *offp);
		switch (fp->type) {
		case BINDER_TYPE_BINDER:
		case BINDER_TYPE_WEAK_BINDER: {
			struct binder_node *node = binder_get_node(proc, fp->binder);
			if (node == NULL) {
				printk(KERN_ERR "binder: transaction release %d bad node %p\n", debug_id, fp->binder);
				break;
			}
			if (binder_debug_mask & BINDER_DEBUG_TRANSACTION)
				printk(KERN_INFO "        node %d u%p\n",
				       node->debug_id, node->ptr);
			binder_dec_node(node, fp->type == BINDER_TYPE_BINDER, 0);
		} break;
		case BINDER_TYPE_HANDLE:
		case BINDER_TYPE_WEAK_HANDLE: {
			struct binder_ref *ref = binder_get_ref(proc, fp->handle);
			if (ref == NULL) {
				printk(KERN_ERR "binder: transaction release %d bad handle %ld\n", debug_id, fp->handle);
				break;
			}
			if (binder_debug_mask & BINDER_DEBUG_TRANSACTION)
				printk(KERN_INFO "        ref %d desc %d (node %d)\n",
				       ref->debug_id, ref->desc, ref->node->debug_id);
			binder_dec_ref(ref, fp->type == BINDER_TYPE_HANDLE);
		} break;

		case BINDER_TYPE_FD:
			if (binder_debug_mask & BINDER_DEBUG_TRANSACTION)
				printk(KERN_INFO "        fd %ld\n", fp->handle);
			if (failed_at)
				task_close_fd(proc, fp->handle);
			break;

		default:
			printk(KERN_ERR "binder: transaction release %d bad object type %lx\n", debug_id, fp->type);
			break;
		}
	}
}

int
binder_thread_write(struct binder_proc *proc, struct binder_thread *thread,
		    void __user *buffer, int size, signed long *consumed)
{
	uint32_t cmd;
	void __user *ptr = buffer + *consumed;
	void __user *end = buffer + size;

	while (ptr < end && thread->return_error == BR_OK) {
		if (get_user(cmd, (uint32_t __user *)ptr))
			return -EFAULT;
		ptr += sizeof(uint32_t);
		if (_IOC_NR(cmd) < ARRAY_SIZE(binder_stats.bc)) {
			binder_stats.bc[_IOC_NR(cmd)]++;
			proc->stats.bc[_IOC_NR(cmd)]++;
			thread->stats.bc[_IOC_NR(cmd)]++;
		}
		switch (cmd) {
		case BC_INCREFS:
		case BC_ACQUIRE:
		case BC_RELEASE:
		case BC_DECREFS: {
			uint32_t target;
			struct binder_ref *ref;
			const char *debug_string;

			if (get_user(target, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			if (target == 0 && binder_context_mgr_node &&
			    (cmd == BC_INCREFS || cmd == BC_ACQUIRE)) {
				ref = binder_get_ref_for_node(proc,
					       binder_context_mgr_node);
				if (ref->desc != target) {
					binder_user_error("binder: %d:"
						"%d tried to acquire "
						"reference to desc 0, "
						"got %d instead\n",
						proc->pid, thread->pid,
						ref->desc);
				}
			} else
				ref = binder_get_ref(proc, target);
			if (ref == NULL) {
				binder_user_error("binder: %d:%d refcou"
					"nt change on invalid ref %d\n",
					proc->pid, thread->pid, target);
				break;
			}
			switch (cmd) {
			case BC_INCREFS:
				debug_string = "IncRefs";
				binder_inc_ref(ref, 0, NULL);
				break;
			case BC_ACQUIRE:
				debug_string = "Acquire";
				binder_inc_ref(ref, 1, NULL);
				break;
			case BC_RELEASE:
				debug_string = "Release";
				binder_dec_ref(ref, 1);
				break;
			case BC_DECREFS:
			default:
				debug_string = "DecRefs";
				binder_dec_ref(ref, 0);
				break;
			}
			if (binder_debug_mask & BINDER_DEBUG_USER_REFS)
				printk(KERN_INFO "binder: %d:%d %s ref %d desc %d s %d w %d for node %d\n",
				       proc->pid, thread->pid, debug_string, ref->debug_id, ref->desc, ref->strong, ref->weak, ref->node->debug_id);
			break;
		}
		case BC_INCREFS_DONE:
		case BC_ACQUIRE_DONE: {
			void __user *node_ptr;
			void *cookie;
			struct binder_node *node;

			if (get_user(node_ptr, (void * __user *)ptr))
				return -EFAULT;
			ptr += sizeof(void *);
			if (get_user(cookie, (void * __user *)ptr))
				return -EFAULT;
			ptr += sizeof(void *);
			node = binder_get_node(proc, node_ptr);
			if (node == NULL) {
				binder_user_error("binder: %d:%d "
					"%s u%p no match\n",
					proc->pid, thread->pid,
					cmd == BC_INCREFS_DONE ?
					"BC_INCREFS_DONE" :
					"BC_ACQUIRE_DONE",
					node_ptr);
				break;
			}
			if (cookie != node->cookie) {
				binder_user_error("binder: %d:%d %s u%p node %d"
					" cookie mismatch %p != %p\n",
					proc->pid, thread->pid,
					cmd == BC_INCREFS_DONE ?
					"BC_INCREFS_DONE" : "BC_ACQUIRE_DONE",
					node_ptr, node->debug_id,
					cookie, node->cookie);
				break;
			}
			if (cmd == BC_ACQUIRE_DONE) {
				if (node->pending_strong_ref == 0) {
					binder_user_error("binder: %d:%d "
						"BC_ACQUIRE_DONE node %d has "
						"no pending acquire request\n",
						proc->pid, thread->pid,
						node->debug_id);
					break;
				}
				node->pending_strong_ref = 0;
			} else {
				if (node->pending_weak_ref == 0) {
					binder_user_error("binder: %d:%d "
						"BC_INCREFS_DONE node %d has "
						"no pending increfs request\n",
						proc->pid, thread->pid,
						node->debug_id);
					break;
				}
				node->pending_weak_ref = 0;
			}
			binder_dec_node(node, cmd == BC_ACQUIRE_DONE, 0);
			if (binder_debug_mask & BINDER_DEBUG_USER_REFS)
				printk(KERN_INFO "binder: %d:%d %s node %d ls %d lw %d\n",
				       proc->pid, thread->pid, cmd == BC_INCREFS_DONE ? "BC_INCREFS_DONE" : "BC_ACQUIRE_DONE", node->debug_id, node->local_strong_refs, node->local_weak_refs);
			break;
		}
		case BC_ATTEMPT_ACQUIRE:
			printk(KERN_ERR "binder: BC_ATTEMPT_ACQUIRE not supported\n");
			return -EINVAL;
		case BC_ACQUIRE_RESULT:
			printk(KERN_ERR "binder: BC_ACQUIRE_RESULT not supported\n");
			return -EINVAL;

		case BC_FREE_BUFFER: {
			void __user *data_ptr;
			struct binder_buffer *buffer;

			if (get_user(data_ptr, (void * __user *)ptr))
				return -EFAULT;
			ptr += sizeof(void *);

			buffer = binder_buffer_lookup(proc, data_ptr);
			if (buffer == NULL) {
				binder_user_error("binder: %d:%d "
					"BC_FREE_BUFFER u%p no match\n",
					proc->pid, thread->pid, data_ptr);
				break;
			}
			if (!buffer->allow_user_free) {
				binder_user_error("binder: %d:%d "
					"BC_FREE_BUFFER u%p matched "
					"unreturned buffer\n",
					proc->pid, thread->pid, data_ptr);
				break;
			}
			if (binder_debug_mask & BINDER_DEBUG_FREE_BUFFER)
				printk(KERN_INFO "binder: %d:%d BC_FREE_BUFFER u%p found buffer %d for %s transaction\n",
				       proc->pid, thread->pid, data_ptr, buffer->debug_id,
				       buffer->transaction ? "active" : "finished");

			if (buffer->transaction) {
				buffer->transaction->buffer = NULL;
				buffer->transaction = NULL;
			}
			if (buffer->async_transaction && buffer->target_node) {
				BUG_ON(!buffer->target_node->has_async_transaction);
				if (list_empty(&buffer->target_node->async_todo))
					buffer->target_node->has_async_transaction = 0;
				else
					list_move_tail(buffer->target_node->async_todo.next, &thread->todo);
			}
			binder_transaction_buffer_release(proc, buffer, NULL);
			binder_free_buf(proc, buffer);
			break;
		}

		case BC_TRANSACTION:
		case BC_REPLY: {
			struct binder_transaction_data tr;

			if (copy_from_user(&tr, ptr, sizeof(tr)))
				return -EFAULT;
			ptr += sizeof(tr);
			binder_transaction(proc, thread, &tr, cmd == BC_REPLY);
			break;
		}

		case BC_REGISTER_LOOPER:
			if (binder_debug_mask & BINDER_DEBUG_THREADS)
				printk(KERN_INFO "binder: %d:%d BC_REGISTER_LOOPER\n",
				       proc->pid, thread->pid);
			if (thread->looper & BINDER_LOOPER_STATE_ENTERED) {
				thread->looper |= BINDER_LOOPER_STATE_INVALID;
				binder_user_error("binder: %d:%d ERROR:"
					" BC_REGISTER_LOOPER called "
					"after BC_ENTER_LOOPER\n",
					proc->pid, thread->pid);
			} else if (proc->requested_threads == 0) {
				thread->looper |= BINDER_LOOPER_STATE_INVALID;
				binder_user_error("binder: %d:%d ERROR:"
					" BC_REGISTER_LOOPER called "
					"without request\n",
					proc->pid, thread->pid);
			} else {
				proc->requested_threads--;
				proc->requested_threads_started++;
			}
			thread->looper |= BINDER_LOOPER_STATE_REGISTERED;
			break;
		case BC_ENTER_LOOPER:
			if (binder_debug_mask & BINDER_DEBUG_THREADS)
				printk(KERN_INFO "binder: %d:%d BC_ENTER_LOOPER\n",
				       proc->pid, thread->pid);
			if (thread->looper & BINDER_LOOPER_STATE_REGISTERED) {
				thread->looper |= BINDER_LOOPER_STATE_INVALID;
				binder_user_error("binder: %d:%d ERROR:"
					" BC_ENTER_LOOPER called after "
					"BC_REGISTER_LOOPER\n",
					proc->pid, thread->pid);
			}
			thread->looper |= BINDER_LOOPER_STATE_ENTERED;
			break;
		case BC_EXIT_LOOPER:
			if (binder_debug_mask & BINDER_DEBUG_THREADS)
				printk(KERN_INFO "binder: %d:%d BC_EXIT_LOOPER\n",
				       proc->pid, thread->pid);
			thread->looper |= BINDER_LOOPER_STATE_EXITED;
			break;

		case BC_REQUEST_DEATH_NOTIFICATION:
		case BC_CLEAR_DEATH_NOTIFICATION: {
			uint32_t target;
			void __user *cookie;
			struct binder_ref *ref;
			struct binder_ref_death *death;

			if (get_user(target, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			if (get_user(cookie, (void __user * __user *)ptr))
				return -EFAULT;
			ptr += sizeof(void *);
			ref = binder_get_ref(proc, target);
			if (ref == NULL) {
				binder_user_error("binder: %d:%d %s "
					"invalid ref %d\n",
					proc->pid, thread->pid,
					cmd == BC_REQUEST_DEATH_NOTIFICATION ?
					"BC_REQUEST_DEATH_NOTIFICATION" :
					"BC_CLEAR_DEATH_NOTIFICATION",
					target);
				break;
			}

			if (binder_debug_mask & BINDER_DEBUG_DEATH_NOTIFICATION)
				printk(KERN_INFO "binder: %d:%d %s %p ref %d desc %d s %d w %d for node %d\n",
				       proc->pid, thread->pid,
				       cmd == BC_REQUEST_DEATH_NOTIFICATION ?
				       "BC_REQUEST_DEATH_NOTIFICATION" :
				       "BC_CLEAR_DEATH_NOTIFICATION",
				       cookie, ref->debug_id, ref->desc,
				       ref->strong, ref->weak, ref->node->debug_id);

			if (cmd == BC_REQUEST_DEATH_NOTIFICATION) {
				if (ref->death) {
					binder_user_error("binder: %d:%"
						"d BC_REQUEST_DEATH_NOTI"
						"FICATION death notific"
						"ation already set\n",
						proc->pid, thread->pid);
					break;
				}
				death = kzalloc(sizeof(*death), GFP_KERNEL);
				if (death == NULL) {
					thread->return_error = BR_ERROR;
					if (binder_debug_mask & BINDER_DEBUG_FAILED_TRANSACTION)
						printk(KERN_INFO "binder: %d:%d "
							"BC_REQUEST_DEATH_NOTIFICATION failed\n",
							proc->pid, thread->pid);
					break;
				}
				binder_stats.obj_created[BINDER_STAT_DEATH]++;
				INIT_LIST_HEAD(&death->work.entry);
				death->cookie = cookie;
				ref->death = death;
				if (ref->node->proc == NULL) {
					ref->death->work.type = BINDER_WORK_DEAD_BINDER;
					if (thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED)) {
						list_add_tail(&ref->death->work.entry, &thread->todo);
					} else {
						list_add_tail(&ref->death->work.entry, &proc->todo);
						wake_up_interruptible(&proc->wait);
					}
				}
			} else {
				if (ref->death == NULL) {
					binder_user_error("binder: %d:%"
						"d BC_CLEAR_DEATH_NOTIFI"
						"CATION death notificat"
						"ion not active\n",
						proc->pid, thread->pid);
					break;
				}
				death = ref->death;
				if (death->cookie != cookie) {
					binder_user_error("binder: %d:%"
						"d BC_CLEAR_DEATH_NOTIFI"
						"CATION death notificat"
						"ion cookie mismatch "
						"%p != %p\n",
						proc->pid, thread->pid,
						death->cookie, cookie);
					break;
				}
				ref->death = NULL;
				if (list_empty(&death->work.entry)) {
					death->work.type = BINDER_WORK_CLEAR_DEATH_NOTIFICATION;
					if (thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED)) {
						list_add_tail(&death->work.entry, &thread->todo);
					} else {
						list_add_tail(&death->work.entry, &proc->todo);
						wake_up_interruptible(&proc->wait);
					}
				} else {
					BUG_ON(death->work.type != BINDER_WORK_DEAD_BINDER);
					death->work.type = BINDER_WORK_DEAD_BINDER_AND_CLEAR;
				}
			}
		} break;
		case BC_DEAD_BINDER_DONE: {
			struct binder_work *w;
			void __user *cookie;
			struct binder_ref_death *death = NULL;
			if (get_user(cookie, (void __user * __user *)ptr))
				return -EFAULT;

			ptr += sizeof(void *);
			list_for_each_entry(w, &proc->delivered_death, entry) {
				struct binder_ref_death *tmp_death = container_of(w, struct binder_ref_death, work);
				if (tmp_death->cookie == cookie) {
					death = tmp_death;
					break;
				}
			}
			if (binder_debug_mask & BINDER_DEBUG_DEAD_BINDER)
				printk(KERN_INFO "binder: %d:%d BC_DEAD_BINDER_DONE %p found %p\n",
				       proc->pid, thread->pid, cookie, death);
			if (death == NULL) {
				binder_user_error("binder: %d:%d BC_DEAD"
					"_BINDER_DONE %p not found\n",
					proc->pid, thread->pid, cookie);
				break;
			}

			list_del_init(&death->work.entry);
			if (death->work.type == BINDER_WORK_DEAD_BINDER_AND_CLEAR) {
				death->work.type = BINDER_WORK_CLEAR_DEATH_NOTIFICATION;
				if (thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED)) {
					list_add_tail(&death->work.entry, &thread->todo);
				} else {
					list_add_tail(&death->work.entry, &proc->todo);
					wake_up_interruptible(&proc->wait);
				}
			}
		} break;

		default:
			printk(KERN_ERR "binder: %d:%d unknown command %d\n", proc->pid, thread->pid, cmd);
			return -EINVAL;
		}
		*consumed = ptr - buffer;
	}
	return 0;
}

void
binder_stat_br(struct binder_proc *proc, struct binder_thread *thread, uint32_t cmd)
{
	if (_IOC_NR(cmd) < ARRAY_SIZE(binder_stats.br)) {
		binder_stats.br[_IOC_NR(cmd)]++;
		proc->stats.br[_IOC_NR(cmd)]++;
		thread->stats.br[_IOC_NR(cmd)]++;
	}
}

static int
binder_has_proc_work(struct binder_proc *proc, struct binder_thread *thread)
{
	return !list_empty(&proc->todo) || (thread->looper & BINDER_LOOPER_STATE_NEED_RETURN);
}

static int
binder_has_thread_work(struct binder_thread *thread)
{
	return !list_empty(&thread->todo) || thread->return_error != BR_OK ||
		(thread->looper & BINDER_LOOPER_STATE_NEED_RETURN);
}

static int
binder_thread_read(struct binder_proc *proc, struct binder_thread *thread,
	void  __user *buffer, int size, signed long *consumed, int non_block)
{
	void __user *ptr = buffer + *consumed;
	void __user *end = buffer + size;

	int ret = 0;
	int wait_for_proc_work;

	if (*consumed == 0) {
		if (put_user(BR_NOOP, (uint32_t __user *)ptr))
			return -EFAULT;
		ptr += sizeof(uint32_t);
	}

retry:
	wait_for_proc_work = thread->transaction_stack == NULL && list_empty(&thread->todo);

	if (thread->return_error != BR_OK && ptr < end) {
		if (thread->return_error2 != BR_OK) {
			if (put_user(thread->return_error2, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			if (ptr == end)
				goto done;
			thread->return_error2 = BR_OK;
		}
		if (put_user(thread->return_error, (uint32_t __user *)ptr))
			return -EFAULT;
		ptr += sizeof(uint32_t);
		thread->return_error = BR_OK;
		goto done;
	}


	thread->looper |= BINDER_LOOPER_STATE_WAITING;
	if (wait_for_proc_work)
		proc->ready_threads++;
	mutex_unlock(&binder_lock);
	if (wait_for_proc_work) {
		if (!(thread->looper & (BINDER_LOOPER_STATE_REGISTERED |
					BINDER_LOOPER_STATE_ENTERED))) {
			binder_user_error("binder: %d:%d ERROR: Thread waiting "
				"for process work before calling BC_REGISTER_"
				"LOOPER or BC_ENTER_LOOPER (state %x)\n",
				proc->pid, thread->pid, thread->looper);
			wait_event_interruptible(binder_user_error_wait, binder_stop_on_user_error < 2);
		}
		binder_set_nice(proc->default_priority);
		if (non_block) {
			if (!binder_has_proc_work(proc, thread))
				ret = -EAGAIN;
		} else
			ret = wait_event_interruptible_exclusive(proc->wait, binder_has_proc_work(proc, thread));
	} else {
		if (non_block) {
			if (!binder_has_thread_work(thread))
				ret = -EAGAIN;
		} else
			ret = wait_event_interruptible(thread->wait, binder_has_thread_work(thread));
	}
	mutex_lock(&binder_lock);
	if (wait_for_proc_work)
		proc->ready_threads--;
	thread->looper &= ~BINDER_LOOPER_STATE_WAITING;

	if (ret)
		return ret;

	while (1) {
		uint32_t cmd;
		struct binder_transaction_data tr;
		struct binder_work *w;
		struct binder_transaction *t = NULL;

		if (!list_empty(&thread->todo))
			w = list_first_entry(&thread->todo, struct binder_work, entry);
		else if (!list_empty(&proc->todo) && wait_for_proc_work)
			w = list_first_entry(&proc->todo, struct binder_work, entry);
		else {
			if (ptr - buffer == 4 && !(thread->looper & BINDER_LOOPER_STATE_NEED_RETURN)) /* no data added */
				goto retry;
			break;
		}

		if (end - ptr < sizeof(tr) + 4)
			break;

		switch (w->type) {
		case BINDER_WORK_TRANSACTION: {
			t = container_of(w, struct binder_transaction, work);
		} break;
		case BINDER_WORK_TRANSACTION_COMPLETE: {
			cmd = BR_TRANSACTION_COMPLETE;
			if (put_user(cmd, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);

			binder_stat_br(proc, thread, cmd);
			if (binder_debug_mask & BINDER_DEBUG_TRANSACTION_COMPLETE)
				printk(KERN_INFO "binder: %d:%d BR_TRANSACTION_COMPLETE\n",
				       proc->pid, thread->pid);

			list_del(&w->entry);
			kfree(w);
			binder_stats.obj_deleted[BINDER_STAT_TRANSACTION_COMPLETE]++;
		} break;
		case BINDER_WORK_NODE: {
			struct binder_node *node = container_of(w, struct binder_node, work);
			uint32_t cmd = BR_NOOP;
			const char *cmd_name;
			int strong = node->internal_strong_refs || node->local_strong_refs;
			int weak = !hlist_empty(&node->refs) || node->local_weak_refs || strong;
			if (weak && !node->has_weak_ref) {
				cmd = BR_INCREFS;
				cmd_name = "BR_INCREFS";
				node->has_weak_ref = 1;
				node->pending_weak_ref = 1;
				node->local_weak_refs++;
			} else if (strong && !node->has_strong_ref) {
				cmd = BR_ACQUIRE;
				cmd_name = "BR_ACQUIRE";
				node->has_strong_ref = 1;
				node->pending_strong_ref = 1;
				node->local_strong_refs++;
			} else if (!strong && node->has_strong_ref) {
				cmd = BR_RELEASE;
				cmd_name = "BR_RELEASE";
				node->has_strong_ref = 0;
			} else if (!weak && node->has_weak_ref) {
				cmd = BR_DECREFS;
				cmd_name = "BR_DECREFS";
				node->has_weak_ref = 0;
			}
			if (cmd != BR_NOOP) {
				if (put_user(cmd, (uint32_t __user *)ptr))
					return -EFAULT;
				ptr += sizeof(uint32_t);
				if (put_user(node->ptr, (void * __user *)ptr))
					return -EFAULT;
				ptr += sizeof(void *);
				if (put_user(node->cookie, (void * __user *)ptr))
					return -EFAULT;
				ptr += sizeof(void *);

				binder_stat_br(proc, thread, cmd);
				if (binder_debug_mask & BINDER_DEBUG_USER_REFS)
					printk(KERN_INFO "binder: %d:%d %s %d u%p c%p\n",
					       proc->pid, thread->pid, cmd_name, node->debug_id, node->ptr, node->cookie);
			} else {
				list_del_init(&w->entry);
				if (!weak && !strong) {
					if (binder_debug_mask & BINDER_DEBUG_INTERNAL_REFS)
						printk(KERN_INFO "binder: %d:%d node %d u%p c%p deleted\n",
						       proc->pid, thread->pid, node->debug_id, node->ptr, node->cookie);
					rb_erase(&node->rb_node, &proc->nodes);
					kfree(node);
					binder_stats.obj_deleted[BINDER_STAT_NODE]++;
				} else {
					if (binder_debug_mask & BINDER_DEBUG_INTERNAL_REFS)
						printk(KERN_INFO "binder: %d:%d node %d u%p c%p state unchanged\n",
						       proc->pid, thread->pid, node->debug_id, node->ptr, node->cookie);
				}
			}
		} break;
		case BINDER_WORK_DEAD_BINDER:
		case BINDER_WORK_DEAD_BINDER_AND_CLEAR:
		case BINDER_WORK_CLEAR_DEATH_NOTIFICATION: {
			struct binder_ref_death *death = container_of(w, struct binder_ref_death, work);
			uint32_t cmd;
			if (w->type == BINDER_WORK_CLEAR_DEATH_NOTIFICATION)
				cmd = BR_CLEAR_DEATH_NOTIFICATION_DONE;
			else
				cmd = BR_DEAD_BINDER;
			if (put_user(cmd, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			if (put_user(death->cookie, (void * __user *)ptr))
				return -EFAULT;
			ptr += sizeof(void *);
			if (binder_debug_mask & BINDER_DEBUG_DEATH_NOTIFICATION)
				printk(KERN_INFO "binder: %d:%d %s %p\n",
				       proc->pid, thread->pid,
				       cmd == BR_DEAD_BINDER ?
				       "BR_DEAD_BINDER" :
				       "BR_CLEAR_DEATH_NOTIFICATION_DONE",
				       death->cookie);

			if (w->type == BINDER_WORK_CLEAR_DEATH_NOTIFICATION) {
				list_del(&w->entry);
				kfree(death);
				binder_stats.obj_deleted[BINDER_STAT_DEATH]++;
			} else
				list_move(&w->entry, &proc->delivered_death);
			if (cmd == BR_DEAD_BINDER)
				goto done; /* DEAD_BINDER notifications can cause transactions */
		} break;
		}

		if (!t)
			continue;

		BUG_ON(t->buffer == NULL);
		if (t->buffer->target_node) {
			struct binder_node *target_node = t->buffer->target_node;
			tr.target.ptr = target_node->ptr;
			tr.cookie =  target_node->cookie;
			t->saved_priority = task_nice(current);
			if (t->priority < target_node->min_priority &&
			    !(t->flags & TF_ONE_WAY))
				binder_set_nice(t->priority);
			else if (!(t->flags & TF_ONE_WAY) ||
				 t->saved_priority > target_node->min_priority)
				binder_set_nice(target_node->min_priority);
			cmd = BR_TRANSACTION;
		} else {
			tr.target.ptr = NULL;
			tr.cookie = NULL;
			cmd = BR_REPLY;
		}
		tr.code = t->code;
		tr.flags = t->flags;
		tr.sender_euid = t->sender_euid;

		if (t->from) {
			struct task_struct *sender = t->from->proc->tsk;
			tr.sender_pid = task_tgid_nr_ns(sender, current->nsproxy->pid_ns);
		} else {
			tr.sender_pid = 0;
		}

		tr.data_size = t->buffer->data_size;
		tr.offsets_size = t->buffer->offsets_size;
		tr.data.ptr.buffer = (void *)t->buffer->data + proc->user_buffer_offset;
		tr.data.ptr.offsets = tr.data.ptr.buffer + ALIGN(t->buffer->data_size, sizeof(void *));

		if (put_user(cmd, (uint32_t __user *)ptr))
			return -EFAULT;
		ptr += sizeof(uint32_t);
		if (copy_to_user(ptr, &tr, sizeof(tr)))
			return -EFAULT;
		ptr += sizeof(tr);

		binder_stat_br(proc, thread, cmd);
		if (binder_debug_mask & BINDER_DEBUG_TRANSACTION)
			printk(KERN_INFO "binder: %d:%d %s %d %d:%d, cmd %d"
				"size %zd-%zd ptr %p-%p\n",
			       proc->pid, thread->pid,
			       (cmd == BR_TRANSACTION) ? "BR_TRANSACTION" : "BR_REPLY",
			       t->debug_id, t->from ? t->from->proc->pid : 0,
			       t->from ? t->from->pid : 0, cmd,
			       t->buffer->data_size, t->buffer->offsets_size,
			       tr.data.ptr.buffer, tr.data.ptr.offsets);

		list_del(&t->work.entry);
		t->buffer->allow_user_free = 1;
		if (cmd == BR_TRANSACTION && !(t->flags & TF_ONE_WAY)) {
			t->to_parent = thread->transaction_stack;
			t->to_thread = thread;
			thread->transaction_stack = t;
		} else {
			t->buffer->transaction = NULL;
			kfree(t);
			binder_stats.obj_deleted[BINDER_STAT_TRANSACTION]++;
		}
		break;
	}

done:

	*consumed = ptr - buffer;
	if (proc->requested_threads + proc->ready_threads == 0 &&
	    proc->requested_threads_started < proc->max_threads &&
	    (thread->looper & (BINDER_LOOPER_STATE_REGISTERED |
	     BINDER_LOOPER_STATE_ENTERED)) /* the user-space code fails to */
	     /*spawn a new thread if we leave this out */) {
		proc->requested_threads++;
		if (binder_debug_mask & BINDER_DEBUG_THREADS)
			printk(KERN_INFO "binder: %d:%d BR_SPAWN_LOOPER\n",
			       proc->pid, thread->pid);
		if (put_user(BR_SPAWN_LOOPER, (uint32_t __user *)buffer))
			return -EFAULT;
	}
	return 0;
}

static void binder_release_work(struct list_head *list)
{
	struct binder_work *w;
	while (!list_empty(list)) {
		w = list_first_entry(list, struct binder_work, entry);
		list_del_init(&w->entry);
		switch (w->type) {
		case BINDER_WORK_TRANSACTION: {
			struct binder_transaction *t = container_of(w, struct binder_transaction, work);
			if (t->buffer->target_node && !(t->flags & TF_ONE_WAY))
				binder_send_failed_reply(t, BR_DEAD_REPLY);
		} break;
		case BINDER_WORK_TRANSACTION_COMPLETE: {
			kfree(w);
			binder_stats.obj_deleted[BINDER_STAT_TRANSACTION_COMPLETE]++;
		} break;
		default:
			break;
		}
	}

}

static struct binder_thread *binder_get_thread(struct binder_proc *proc)
{
	struct binder_thread *thread = NULL;
	struct rb_node *parent = NULL;
	struct rb_node **p = &proc->threads.rb_node;

	while (*p) {
		parent = *p;
		thread = rb_entry(parent, struct binder_thread, rb_node);

		if (current->pid < thread->pid)
			p = &(*p)->rb_left;
		else if (current->pid > thread->pid)
			p = &(*p)->rb_right;
		else
			break;
	}
	if (*p == NULL) {
		thread = kzalloc(sizeof(*thread), GFP_KERNEL);
		if (thread == NULL)
			return NULL;
		binder_stats.obj_created[BINDER_STAT_THREAD]++;
		thread->proc = proc;
		thread->pid = current->pid;
		init_waitqueue_head(&thread->wait);
		INIT_LIST_HEAD(&thread->todo);
		rb_link_node(&thread->rb_node, parent, p);
		rb_insert_color(&thread->rb_node, &proc->threads);
		thread->looper |= BINDER_LOOPER_STATE_NEED_RETURN;
		thread->return_error = BR_OK;
		thread->return_error2 = BR_OK;
	}
	return thread;
}

static int binder_free_thread(struct binder_proc *proc, struct binder_thread *thread)
{
	struct binder_transaction *t;
	struct binder_transaction *send_reply = NULL;
	int active_transactions = 0;

	rb_erase(&thread->rb_node, &proc->threads);
	t = thread->transaction_stack;
	if (t && t->to_thread == thread)
		send_reply = t;
	while (t) {
		active_transactions++;
		if (binder_debug_mask & BINDER_DEBUG_DEAD_TRANSACTION)
			printk(KERN_INFO "binder: release %d:%d transaction %d %s, still active\n",
			       proc->pid, thread->pid, t->debug_id, (t->to_thread == thread) ? "in" : "out");
		if (t->to_thread == thread) {
			t->to_proc = NULL;
			t->to_thread = NULL;
			if (t->buffer) {
				t->buffer->transaction = NULL;
				t->buffer = NULL;
			}
			t = t->to_parent;
		} else if (t->from == thread) {
			t->from = NULL;
			t = t->from_parent;
		} else
			BUG();
	}
	if (send_reply)
		binder_send_failed_reply(send_reply, BR_DEAD_REPLY);
	binder_release_work(&thread->todo);
	kfree(thread);
	binder_stats.obj_deleted[BINDER_STAT_THREAD]++;
	return active_transactions;
}

static unsigned int binder_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct binder_proc *proc = filp->private_data;
	struct binder_thread *thread = NULL;
	int wait_for_proc_work;

	mutex_lock(&binder_lock);
	thread = binder_get_thread(proc);

	wait_for_proc_work = thread->transaction_stack == NULL &&
		list_empty(&thread->todo) && thread->return_error == BR_OK;
	mutex_unlock(&binder_lock);

	if (wait_for_proc_work) {
		if (binder_has_proc_work(proc, thread))
			return POLLIN;
		poll_wait(filp, &proc->wait, wait);
		if (binder_has_proc_work(proc, thread))
			return POLLIN;
	} else {
		if (binder_has_thread_work(thread))
			return POLLIN;
		poll_wait(filp, &thread->wait, wait);
		if (binder_has_thread_work(thread))
			return POLLIN;
	}
	return 0;
}

static long binder_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret;
	struct binder_proc *proc = filp->private_data;
	struct binder_thread *thread;
	unsigned int size = _IOC_SIZE(cmd);
	void __user *ubuf = (void __user *)arg;

	/*printk(KERN_INFO "binder_ioctl: %d:%d %x %lx\n", proc->pid, current->pid, cmd, arg);*/

	ret = wait_event_interruptible(binder_user_error_wait, binder_stop_on_user_error < 2);
	if (ret)
		return ret;

	mutex_lock(&binder_lock);
	thread = binder_get_thread(proc);
	if (thread == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	switch (cmd) {
	case BINDER_WRITE_READ: {
		struct binder_write_read bwr;
		if (size != sizeof(struct binder_write_read)) {
			ret = -EINVAL;
			goto err;
		}
		if (copy_from_user(&bwr, ubuf, sizeof(bwr))) {
			ret = -EFAULT;
			goto err;
		}
		if (binder_debug_mask & BINDER_DEBUG_READ_WRITE)
			printk(KERN_INFO "binder: %d:%d write %ld at %08lx, read %ld at %08lx\n",
			       proc->pid, thread->pid, bwr.write_size, bwr.write_buffer, bwr.read_size, bwr.read_buffer);
		if (bwr.write_size > 0) {
			ret = binder_thread_write(proc, thread, (void __user *)bwr.write_buffer, bwr.write_size, &bwr.write_consumed);
			if (ret < 0) {
				bwr.read_consumed = 0;
				if (copy_to_user(ubuf, &bwr, sizeof(bwr)))
					ret = -EFAULT;
				goto err;
			}
		}
		if (bwr.read_size > 0) {
			ret = binder_thread_read(proc, thread, (void __user *)bwr.read_buffer, bwr.read_size, &bwr.read_consumed, filp->f_flags & O_NONBLOCK);
			if (!list_empty(&proc->todo))
				wake_up_interruptible(&proc->wait);
			if (ret < 0) {
				if (copy_to_user(ubuf, &bwr, sizeof(bwr)))
					ret = -EFAULT;
				goto err;
			}
		}
		if (binder_debug_mask & BINDER_DEBUG_READ_WRITE)
			printk(KERN_INFO "binder: %d:%d wrote %ld of %ld, read return %ld of %ld\n",
			       proc->pid, thread->pid, bwr.write_consumed, bwr.write_size, bwr.read_consumed, bwr.read_size);
		if (copy_to_user(ubuf, &bwr, sizeof(bwr))) {
			ret = -EFAULT;
			goto err;
		}
		break;
	}
	case BINDER_SET_MAX_THREADS:
		if (copy_from_user(&proc->max_threads, ubuf, sizeof(proc->max_threads))) {
			ret = -EINVAL;
			goto err;
		}
		break;
	case BINDER_SET_CONTEXT_MGR:
		if (binder_context_mgr_node != NULL) {
			printk(KERN_ERR "binder: BINDER_SET_CONTEXT_MGR already set\n");
			ret = -EBUSY;
			goto err;
		}
		if (binder_context_mgr_uid != -1) {
			if (binder_context_mgr_uid != current->cred->euid) {
				printk(KERN_ERR "binder: BINDER_SET_"
				       "CONTEXT_MGR bad uid %d != %d\n",
				       current->cred->euid,
				       binder_context_mgr_uid);
				ret = -EPERM;
				goto err;
			}
		} else
			binder_context_mgr_uid = current->cred->euid;
		binder_context_mgr_node = binder_new_node(proc, NULL, NULL);
		if (binder_context_mgr_node == NULL) {
			ret = -ENOMEM;
			goto err;
		}
		binder_context_mgr_node->local_weak_refs++;
		binder_context_mgr_node->local_strong_refs++;
		binder_context_mgr_node->has_strong_ref = 1;
		binder_context_mgr_node->has_weak_ref = 1;
		break;
	case BINDER_THREAD_EXIT:
		if (binder_debug_mask & BINDER_DEBUG_THREADS)
			printk(KERN_INFO "binder: %d:%d exit\n",
			       proc->pid, thread->pid);
		binder_free_thread(proc, thread);
		thread = NULL;
		break;
	case BINDER_VERSION:
		if (size != sizeof(struct binder_version)) {
			ret = -EINVAL;
			goto err;
		}
		if (put_user(BINDER_CURRENT_PROTOCOL_VERSION, &((struct binder_version *)ubuf)->protocol_version)) {
			ret = -EINVAL;
			goto err;
		}
		break;
	default:
		ret = -EINVAL;
		goto err;
	}
	ret = 0;
err:
	if (thread)
		thread->looper &= ~BINDER_LOOPER_STATE_NEED_RETURN;
	mutex_unlock(&binder_lock);
	wait_event_interruptible(binder_user_error_wait, binder_stop_on_user_error < 2);
	if (ret && ret != -ERESTARTSYS)
		printk(KERN_INFO "binder: %d:%d ioctl %x %lx returned %d\n", proc->pid, current->pid, cmd, arg, ret);
	return ret;
}

static void binder_vma_open(struct vm_area_struct *vma)
{
	struct binder_proc *proc = vma->vm_private_data;
	if (binder_debug_mask & BINDER_DEBUG_OPEN_CLOSE)
		printk(KERN_INFO
			"binder: %d open vm area %lx-%lx (%ld K) vma %lx pagep %lx\n",
			proc->pid, vma->vm_start, vma->vm_end,
			(vma->vm_end - vma->vm_start) / SZ_1K, vma->vm_flags,
			(unsigned long)pgprot_val(vma->vm_page_prot));
	dump_stack();
}

static void binder_vma_close(struct vm_area_struct *vma)
{
	struct binder_proc *proc = vma->vm_private_data;
	if (binder_debug_mask & BINDER_DEBUG_OPEN_CLOSE)
		printk(KERN_INFO
			"binder: %d close vm area %lx-%lx (%ld K) vma %lx pagep %lx\n",
			proc->pid, vma->vm_start, vma->vm_end,
			(vma->vm_end - vma->vm_start) / SZ_1K, vma->vm_flags,
			(unsigned long)pgprot_val(vma->vm_page_prot));
	proc->vma = NULL;
	binder_defer_work(proc, BINDER_DEFERRED_PUT_FILES);
}

static struct vm_operations_struct binder_vm_ops = {
	.open = binder_vma_open,
	.close = binder_vma_close,
};

static int binder_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret;
	struct vm_struct *area;
	struct binder_proc *proc = filp->private_data;
	const char *failure_string;
	struct binder_buffer *buffer;

	if ((vma->vm_end - vma->vm_start) > SZ_4M)
		vma->vm_end = vma->vm_start + SZ_4M;

	if (binder_debug_mask & BINDER_DEBUG_OPEN_CLOSE)
		printk(KERN_INFO
			"binder_mmap: %d %lx-%lx (%ld K) vma %lx pagep %lx\n",
			proc->pid, vma->vm_start, vma->vm_end,
			(vma->vm_end - vma->vm_start) / SZ_1K, vma->vm_flags,
			(unsigned long)pgprot_val(vma->vm_page_prot));

	if (vma->vm_flags & FORBIDDEN_MMAP_FLAGS) {
		ret = -EPERM;
		failure_string = "bad vm_flags";
		goto err_bad_arg;
	}
	vma->vm_flags = (vma->vm_flags | VM_DONTCOPY) & ~VM_MAYWRITE;

	if (proc->buffer) {
		ret = -EBUSY;
		failure_string = "already mapped";
		goto err_already_mapped;
	}

	area = get_vm_area(vma->vm_end - vma->vm_start, VM_IOREMAP);
	if (area == NULL) {
		ret = -ENOMEM;
		failure_string = "get_vm_area";
		goto err_get_vm_area_failed;
	}
	proc->buffer = area->addr;
	proc->user_buffer_offset = vma->vm_start - (uintptr_t)proc->buffer;

#ifdef CONFIG_CPU_CACHE_VIPT
	if (cache_is_vipt_aliasing()) {
		while (CACHE_COLOUR((vma->vm_start ^ (uint32_t)proc->buffer))) {
			printk(KERN_INFO "binder_mmap: %d %lx-%lx maps %p bad alignment\n", proc->pid, vma->vm_start, vma->vm_end, proc->buffer);
			vma->vm_start += PAGE_SIZE;
		}
	}
#endif
	proc->pages = kzalloc(sizeof(proc->pages[0]) * ((vma->vm_end - vma->vm_start) / PAGE_SIZE), GFP_KERNEL);
	if (proc->pages == NULL) {
		ret = -ENOMEM;
		failure_string = "alloc page array";
		goto err_alloc_pages_failed;
	}
	proc->buffer_size = vma->vm_end - vma->vm_start;

	vma->vm_ops = &binder_vm_ops;
	vma->vm_private_data = proc;

	if (binder_update_page_range(proc, 1, proc->buffer, proc->buffer + PAGE_SIZE, vma)) {
		ret = -ENOMEM;
		failure_string = "alloc small buf";
		goto err_alloc_small_buf_failed;
	}
	buffer = proc->buffer;
	INIT_LIST_HEAD(&proc->buffers);
	list_add(&buffer->entry, &proc->buffers);
	buffer->free = 1;
	binder_insert_free_buffer(proc, buffer);
	proc->free_async_space = proc->buffer_size / 2;
	barrier();
	proc->files = get_files_struct(current);
	proc->vma = vma;

	/*printk(KERN_INFO "binder_mmap: %d %lx-%lx maps %p\n", proc->pid, vma->vm_start, vma->vm_end, proc->buffer);*/
	return 0;

err_alloc_small_buf_failed:
	kfree(proc->pages);
	proc->pages = NULL;
err_alloc_pages_failed:
	vfree(proc->buffer);
	proc->buffer = NULL;
err_get_vm_area_failed:
err_already_mapped:
err_bad_arg:
	printk(KERN_ERR "binder_mmap: %d %lx-%lx %s failed %d\n", proc->pid, vma->vm_start, vma->vm_end, failure_string, ret);
	return ret;
}

static int binder_open(struct inode *nodp, struct file *filp)
{
	struct binder_proc *proc;

	if (binder_debug_mask & BINDER_DEBUG_OPEN_CLOSE)
		printk(KERN_INFO "binder_open: %d:%d\n", current->group_leader->pid, current->pid);

	proc = kzalloc(sizeof(*proc), GFP_KERNEL);
	if (proc == NULL)
		return -ENOMEM;
	get_task_struct(current);
	proc->tsk = current;
	INIT_LIST_HEAD(&proc->todo);
	init_waitqueue_head(&proc->wait);
	proc->default_priority = task_nice(current);
	mutex_lock(&binder_lock);
	binder_stats.obj_created[BINDER_STAT_PROC]++;
	hlist_add_head(&proc->proc_node, &binder_procs);
	proc->pid = current->group_leader->pid;
	INIT_LIST_HEAD(&proc->delivered_death);
	filp->private_data = proc;
	mutex_unlock(&binder_lock);

	if (binder_proc_dir_entry_proc) {
		char strbuf[11];
		snprintf(strbuf, sizeof(strbuf), "%u", proc->pid);
		remove_proc_entry(strbuf, binder_proc_dir_entry_proc);
		create_proc_read_entry(strbuf, S_IRUGO, binder_proc_dir_entry_proc, binder_read_proc_proc, proc);
	}

	return 0;
}

static int binder_flush(struct file *filp, fl_owner_t id)
{
	struct binder_proc *proc = filp->private_data;

	binder_defer_work(proc, BINDER_DEFERRED_FLUSH);

	return 0;
}

static void binder_deferred_flush(struct binder_proc *proc)
{
	struct rb_node *n;
	int wake_count = 0;
	for (n = rb_first(&proc->threads); n != NULL; n = rb_next(n)) {
		struct binder_thread *thread = rb_entry(n, struct binder_thread, rb_node);
		thread->looper |= BINDER_LOOPER_STATE_NEED_RETURN;
		if (thread->looper & BINDER_LOOPER_STATE_WAITING) {
			wake_up_interruptible(&thread->wait);
			wake_count++;
		}
	}
	wake_up_interruptible_all(&proc->wait);

	if (binder_debug_mask & BINDER_DEBUG_OPEN_CLOSE)
		printk(KERN_INFO "binder_flush: %d woke %d threads\n", proc->pid, wake_count);
}

static int binder_release(struct inode *nodp, struct file *filp)
{
	struct binder_proc *proc = filp->private_data;
	if (binder_proc_dir_entry_proc) {
		char strbuf[11];
		snprintf(strbuf, sizeof(strbuf), "%u", proc->pid);
		remove_proc_entry(strbuf, binder_proc_dir_entry_proc);
	}

	binder_defer_work(proc, BINDER_DEFERRED_RELEASE);
	
	return 0;
}

static void binder_deferred_release(struct binder_proc *proc)
{
	struct hlist_node *pos;
	struct binder_transaction *t;
	struct rb_node *n;
	int threads, nodes, incoming_refs, outgoing_refs, buffers, active_transactions, page_count;

	BUG_ON(proc->vma);
	BUG_ON(proc->files);

	hlist_del(&proc->proc_node);
	if (binder_context_mgr_node && binder_context_mgr_node->proc == proc) {
		if (binder_debug_mask & BINDER_DEBUG_DEAD_BINDER)
			printk(KERN_INFO "binder_release: %d context_mgr_node gone\n", proc->pid);
		binder_context_mgr_node = NULL;
	}

	threads = 0;
	active_transactions = 0;
	while ((n = rb_first(&proc->threads))) {
		struct binder_thread *thread = rb_entry(n, struct binder_thread, rb_node);
		threads++;
		active_transactions += binder_free_thread(proc, thread);
	}
	nodes = 0;
	incoming_refs = 0;
	while ((n = rb_first(&proc->nodes))) {
		struct binder_node *node = rb_entry(n, struct binder_node, rb_node);

		nodes++;
		rb_erase(&node->rb_node, &proc->nodes);
		list_del_init(&node->work.entry);
		if (hlist_empty(&node->refs)) {
			kfree(node);
			binder_stats.obj_deleted[BINDER_STAT_NODE]++;
		} else {
			struct binder_ref *ref;
			int death = 0;

			node->proc = NULL;
			node->local_strong_refs = 0;
			node->local_weak_refs = 0;
			hlist_add_head(&node->dead_node, &binder_dead_nodes);

			hlist_for_each_entry(ref, pos, &node->refs, node_entry) {
				incoming_refs++;
				if (ref->death) {
					death++;
					if (list_empty(&ref->death->work.entry)) {
						ref->death->work.type = BINDER_WORK_DEAD_BINDER;
						list_add_tail(&ref->death->work.entry, &ref->proc->todo);
						wake_up_interruptible(&ref->proc->wait);
					} else
						BUG();
				}
			}
			if (binder_debug_mask & BINDER_DEBUG_DEAD_BINDER)
				printk(KERN_INFO "binder: node %d now dead, refs %d, death %d\n", node->debug_id, incoming_refs, death);
		}
	}
	outgoing_refs = 0;
	while ((n = rb_first(&proc->refs_by_desc))) {
		struct binder_ref *ref = rb_entry(n, struct binder_ref, rb_node_desc);
		outgoing_refs++;
		binder_delete_ref(ref);
	}
	binder_release_work(&proc->todo);
	buffers = 0;

	while ((n = rb_first(&proc->allocated_buffers))) {
		struct binder_buffer *buffer = rb_entry(n, struct binder_buffer, rb_node);
		t = buffer->transaction;
		if (t) {
			t->buffer = NULL;
			buffer->transaction = NULL;
			printk(KERN_ERR "binder: release proc %d, transaction %d, not freed\n", proc->pid, t->debug_id);
			/*BUG();*/
		}
		binder_free_buf(proc, buffer);
		buffers++;
	}

	binder_stats.obj_deleted[BINDER_STAT_PROC]++;

	page_count = 0;
	if (proc->pages) {
		int i;
		for (i = 0; i < proc->buffer_size / PAGE_SIZE; i++) {
			if (proc->pages[i]) {
				if (binder_debug_mask & BINDER_DEBUG_BUFFER_ALLOC)
					printk(KERN_INFO "binder_release: %d: page %d at %p not freed\n", proc->pid, i, proc->buffer + i * PAGE_SIZE);
				__free_page(proc->pages[i]);
				page_count++;
			}
		}
		kfree(proc->pages);
		vfree(proc->buffer);
	}

	put_task_struct(proc->tsk);

	if (binder_debug_mask & BINDER_DEBUG_OPEN_CLOSE)
		printk(KERN_INFO "binder_release: %d threads %d, nodes %d (ref %d), refs %d, active transactions %d, buffers %d, pages %d\n",
		       proc->pid, threads, nodes, incoming_refs, outgoing_refs, active_transactions, buffers, page_count);

	kfree(proc);
}

static void binder_deferred_func(struct work_struct *work)
{
	struct binder_proc *proc;
	struct files_struct *files;

	int defer;
	do {
		mutex_lock(&binder_lock);
		mutex_lock(&binder_deferred_lock);
		if (!hlist_empty(&binder_deferred_list)) {
			proc = hlist_entry(binder_deferred_list.first,
					struct binder_proc, deferred_work_node);
			hlist_del_init(&proc->deferred_work_node);
			defer = proc->deferred_work;
			proc->deferred_work = 0;
		} else {
			proc = NULL;
			defer = 0;
		}
		mutex_unlock(&binder_deferred_lock);

		files = NULL;
		if (defer & BINDER_DEFERRED_PUT_FILES)
			if ((files = proc->files))
				proc->files = NULL;

		if (defer & BINDER_DEFERRED_FLUSH)
			binder_deferred_flush(proc);

		if (defer & BINDER_DEFERRED_RELEASE)
			binder_deferred_release(proc); /* frees proc */
	
		mutex_unlock(&binder_lock);
		if (files)
			put_files_struct(files);
	} while (proc);
}
static DECLARE_WORK(binder_deferred_work, binder_deferred_func);

static void binder_defer_work(struct binder_proc *proc, int defer)
{
	mutex_lock(&binder_deferred_lock);
	proc->deferred_work |= defer;
	if (hlist_unhashed(&proc->deferred_work_node)) {
		hlist_add_head(&proc->deferred_work_node,
				&binder_deferred_list);
		schedule_work(&binder_deferred_work);
	}
	mutex_unlock(&binder_deferred_lock);
}

static char *print_binder_transaction(char *buf, char *end, const char *prefix, struct binder_transaction *t)
{
	buf += snprintf(buf, end - buf, "%s %d: %p from %d:%d to %d:%d code %x flags %x pri %ld r%d",
			prefix, t->debug_id, t, t->from ? t->from->proc->pid : 0,
			t->from ? t->from->pid : 0,
			t->to_proc ? t->to_proc->pid : 0,
			t->to_thread ? t->to_thread->pid : 0,
			t->code, t->flags, t->priority, t->need_reply);
	if (buf >= end)
		return buf;
	if (t->buffer == NULL) {
		buf += snprintf(buf, end - buf, " buffer free\n");
		return buf;
	}
	if (t->buffer->target_node) {
		buf += snprintf(buf, end - buf, " node %d",
				t->buffer->target_node->debug_id);
		if (buf >= end)
			return buf;
	}
	buf += snprintf(buf, end - buf, " size %zd:%zd data %p\n",
			t->buffer->data_size, t->buffer->offsets_size,
			t->buffer->data);
	return buf;
}

static char *print_binder_buffer(char *buf, char *end, const char *prefix, struct binder_buffer *buffer)
{
	buf += snprintf(buf, end - buf, "%s %d: %p size %zd:%zd %s\n",
			prefix, buffer->debug_id, buffer->data,
			buffer->data_size, buffer->offsets_size,
			buffer->transaction ? "active" : "delivered");
	return buf;
}

static char *print_binder_work(char *buf, char *end, const char *prefix,
	const char *transaction_prefix, struct binder_work *w)
{
	struct binder_node *node;
	struct binder_transaction *t;

	switch (w->type) {
	case BINDER_WORK_TRANSACTION:
		t = container_of(w, struct binder_transaction, work);
		buf = print_binder_transaction(buf, end, transaction_prefix, t);
		break;
	case BINDER_WORK_TRANSACTION_COMPLETE:
		buf += snprintf(buf, end - buf,
				"%stransaction complete\n", prefix);
		break;
	case BINDER_WORK_NODE:
		node = container_of(w, struct binder_node, work);
		buf += snprintf(buf, end - buf, "%snode work %d: u%p c%p\n",
				prefix, node->debug_id, node->ptr, node->cookie);
		break;
	case BINDER_WORK_DEAD_BINDER:
		buf += snprintf(buf, end - buf, "%shas dead binder\n", prefix);
		break;
	case BINDER_WORK_DEAD_BINDER_AND_CLEAR:
		buf += snprintf(buf, end - buf,
				"%shas cleared dead binder\n", prefix);
		break;
	case BINDER_WORK_CLEAR_DEATH_NOTIFICATION:
		buf += snprintf(buf, end - buf,
				"%shas cleared death notification\n", prefix);
		break;
	default:
		buf += snprintf(buf, end - buf, "%sunknown work: type %d\n",
				prefix, w->type);
		break;
	}
	return buf;
}

static char *print_binder_thread(char *buf, char *end, struct binder_thread *thread, int print_always)
{
	struct binder_transaction *t;
	struct binder_work *w;
	char *start_buf = buf;
	char *header_buf;

	buf += snprintf(buf, end - buf, "  thread %d: l %02x\n", thread->pid, thread->looper);
	header_buf = buf;
	t = thread->transaction_stack;
	while (t) {
		if (buf >= end)
			break;
		if (t->from == thread) {
			buf = print_binder_transaction(buf, end, "    outgoing transaction", t);
			t = t->from_parent;
		} else if (t->to_thread == thread) {
			buf = print_binder_transaction(buf, end, "    incoming transaction", t);
			t = t->to_parent;
		} else {
			buf = print_binder_transaction(buf, end, "    bad transaction", t);
			t = NULL;
		}
	}
	list_for_each_entry(w, &thread->todo, entry) {
		if (buf >= end)
			break;
		buf = print_binder_work(buf, end, "    ",
					"    pending transaction", w);
	}
	if (!print_always && buf == header_buf)
		buf = start_buf;
	return buf;
}

static char *print_binder_node(char *buf, char *end, struct binder_node *node)
{
	struct binder_ref *ref;
	struct hlist_node *pos;
	struct binder_work *w;
	int count;
	count = 0;
	hlist_for_each_entry(ref, pos, &node->refs, node_entry)
		count++;

	buf += snprintf(buf, end - buf, "  node %d: u%p c%p hs %d hw %d ls %d lw %d is %d iw %d",
			node->debug_id, node->ptr, node->cookie,
			node->has_strong_ref, node->has_weak_ref,
			node->local_strong_refs, node->local_weak_refs,
			node->internal_strong_refs, count);
	if (buf >= end)
		return buf;
	if (count) {
		buf += snprintf(buf, end - buf, " proc");
		if (buf >= end)
			return buf;
		hlist_for_each_entry(ref, pos, &node->refs, node_entry) {
			buf += snprintf(buf, end - buf, " %d", ref->proc->pid);
			if (buf >= end)
				return buf;
		}
	}
	buf += snprintf(buf, end - buf, "\n");
	list_for_each_entry(w, &node->async_todo, entry) {
		if (buf >= end)
			break;
		buf = print_binder_work(buf, end, "    ",
					"    pending async transaction", w);
	}
	return buf;
}

static char *print_binder_ref(char *buf, char *end, struct binder_ref *ref)
{
	buf += snprintf(buf, end - buf, "  ref %d: desc %d %snode %d s %d w %d d %p\n",
			ref->debug_id, ref->desc, ref->node->proc ? "" : "dead ",
			ref->node->debug_id, ref->strong, ref->weak, ref->death);
	return buf;
}

static char *print_binder_proc(char *buf, char *end, struct binder_proc *proc, int print_all)
{
	struct binder_work *w;
	struct rb_node *n;
	char *start_buf = buf;
	char *header_buf;

	buf += snprintf(buf, end - buf, "proc %d\n", proc->pid);
	header_buf = buf;

	for (n = rb_first(&proc->threads); n != NULL && buf < end; n = rb_next(n))
		buf = print_binder_thread(buf, end, rb_entry(n, struct binder_thread, rb_node), print_all);
	for (n = rb_first(&proc->nodes); n != NULL && buf < end; n = rb_next(n)) {
		struct binder_node *node = rb_entry(n, struct binder_node, rb_node);
		if (print_all || node->has_async_transaction)
			buf = print_binder_node(buf, end, node);
	}
	if (print_all) {
		for (n = rb_first(&proc->refs_by_desc); n != NULL && buf < end; n = rb_next(n))
			buf = print_binder_ref(buf, end, rb_entry(n, struct binder_ref, rb_node_desc));
	}
	for (n = rb_first(&proc->allocated_buffers); n != NULL && buf < end; n = rb_next(n))
		buf = print_binder_buffer(buf, end, "  buffer", rb_entry(n, struct binder_buffer, rb_node));
	list_for_each_entry(w, &proc->todo, entry) {
		if (buf >= end)
			break;
		buf = print_binder_work(buf, end, "  ",
					"  pending transaction", w);
	}
	list_for_each_entry(w, &proc->delivered_death, entry) {
		if (buf >= end)
			break;
		buf += snprintf(buf, end - buf, "  has delivered dead binder\n");
		break;
	}
	if (!print_all && buf == header_buf)
		buf = start_buf;
	return buf;
}

static const char *binder_return_strings[] = {
	"BR_ERROR",
	"BR_OK",
	"BR_TRANSACTION",
	"BR_REPLY",
	"BR_ACQUIRE_RESULT",
	"BR_DEAD_REPLY",
	"BR_TRANSACTION_COMPLETE",
	"BR_INCREFS",
	"BR_ACQUIRE",
	"BR_RELEASE",
	"BR_DECREFS",
	"BR_ATTEMPT_ACQUIRE",
	"BR_NOOP",
	"BR_SPAWN_LOOPER",
	"BR_FINISHED",
	"BR_DEAD_BINDER",
	"BR_CLEAR_DEATH_NOTIFICATION_DONE",
	"BR_FAILED_REPLY"
};

static const char *binder_command_strings[] = {
	"BC_TRANSACTION",
	"BC_REPLY",
	"BC_ACQUIRE_RESULT",
	"BC_FREE_BUFFER",
	"BC_INCREFS",
	"BC_ACQUIRE",
	"BC_RELEASE",
	"BC_DECREFS",
	"BC_INCREFS_DONE",
	"BC_ACQUIRE_DONE",
	"BC_ATTEMPT_ACQUIRE",
	"BC_REGISTER_LOOPER",
	"BC_ENTER_LOOPER",
	"BC_EXIT_LOOPER",
	"BC_REQUEST_DEATH_NOTIFICATION",
	"BC_CLEAR_DEATH_NOTIFICATION",
	"BC_DEAD_BINDER_DONE"
};

static const char *binder_objstat_strings[] = {
	"proc",
	"thread",
	"node",
	"ref",
	"death",
	"transaction",
	"transaction_complete"
};

static char *print_binder_stats(char *buf, char *end, const char *prefix, struct binder_stats *stats)
{
	int i;

	BUILD_BUG_ON(ARRAY_SIZE(stats->bc) != ARRAY_SIZE(binder_command_strings));
	for (i = 0; i < ARRAY_SIZE(stats->bc); i++) {
		if (stats->bc[i])
			buf += snprintf(buf, end - buf, "%s%s: %d\n", prefix,
					binder_command_strings[i], stats->bc[i]);
		if (buf >= end)
			return buf;
	}

	BUILD_BUG_ON(ARRAY_SIZE(stats->br) != ARRAY_SIZE(binder_return_strings));
	for (i = 0; i < ARRAY_SIZE(stats->br); i++) {
		if (stats->br[i])
			buf += snprintf(buf, end - buf, "%s%s: %d\n", prefix,
					binder_return_strings[i], stats->br[i]);
		if (buf >= end)
			return buf;
	}

	BUILD_BUG_ON(ARRAY_SIZE(stats->obj_created) != ARRAY_SIZE(binder_objstat_strings));
	BUILD_BUG_ON(ARRAY_SIZE(stats->obj_created) != ARRAY_SIZE(stats->obj_deleted));
	for (i = 0; i < ARRAY_SIZE(stats->obj_created); i++) {
		if (stats->obj_created[i] || stats->obj_deleted[i])
			buf += snprintf(buf, end - buf, "%s%s: active %d total %d\n", prefix,
					binder_objstat_strings[i],
					stats->obj_created[i] - stats->obj_deleted[i],
					stats->obj_created[i]);
		if (buf >= end)
			return buf;
	}
	return buf;
}

static char *print_binder_proc_stats(char *buf, char *end, struct binder_proc *proc)
{
	struct binder_work *w;
	struct rb_node *n;
	int count, strong, weak;

	buf += snprintf(buf, end - buf, "proc %d\n", proc->pid);
	if (buf >= end)
		return buf;
	count = 0;
	for (n = rb_first(&proc->threads); n != NULL; n = rb_next(n))
		count++;
	buf += snprintf(buf, end - buf, "  threads: %d\n", count);
	if (buf >= end)
		return buf;
	buf += snprintf(buf, end - buf, "  requested threads: %d+%d/%d\n"
			"  ready threads %d\n"
			"  free async space %zd\n", proc->requested_threads,
			proc->requested_threads_started, proc->max_threads,
			proc->ready_threads, proc->free_async_space);
	if (buf >= end)
		return buf;
	count = 0;
	for (n = rb_first(&proc->nodes); n != NULL; n = rb_next(n))
		count++;
	buf += snprintf(buf, end - buf, "  nodes: %d\n", count);
	if (buf >= end)
		return buf;
	count = 0;
	strong = 0;
	weak = 0;
	for (n = rb_first(&proc->refs_by_desc); n != NULL; n = rb_next(n)) {
		struct binder_ref *ref = rb_entry(n, struct binder_ref, rb_node_desc);
		count++;
		strong += ref->strong;
		weak += ref->weak;
	}
	buf += snprintf(buf, end - buf, "  refs: %d s %d w %d\n", count, strong, weak);
	if (buf >= end)
		return buf;

	count = 0;
	for (n = rb_first(&proc->allocated_buffers); n != NULL; n = rb_next(n))
		count++;
	buf += snprintf(buf, end - buf, "  buffers: %d\n", count);
	if (buf >= end)
		return buf;

	count = 0;
	list_for_each_entry(w, &proc->todo, entry) {
		switch (w->type) {
		case BINDER_WORK_TRANSACTION:
			count++;
			break;
		default:
			break;
		}
	}
	buf += snprintf(buf, end - buf, "  pending transactions: %d\n", count);
	if (buf >= end)
		return buf;

	buf = print_binder_stats(buf, end, "  ", &proc->stats);

	return buf;
}


static int binder_read_proc_state(
	char *page, char **start, off_t off, int count, int *eof, void *data)
{
	struct binder_proc *proc;
	struct hlist_node *pos;
	struct binder_node *node;
	int len = 0;
	char *buf = page;
	char *end = page + PAGE_SIZE;
	int do_lock = !binder_debug_no_lock;

	if (off)
		return 0;

	if (do_lock)
		mutex_lock(&binder_lock);

	buf += snprintf(buf, end - buf, "binder state:\n");

	if (!hlist_empty(&binder_dead_nodes))
		buf += snprintf(buf, end - buf, "dead nodes:\n");
	hlist_for_each_entry(node, pos, &binder_dead_nodes, dead_node) {
		if (buf >= end)
			break;
		buf = print_binder_node(buf, end, node);
	}

	hlist_for_each_entry(proc, pos, &binder_procs, proc_node) {
		if (buf >= end)
			break;
		buf = print_binder_proc(buf, end, proc, 1);
	}
	if (do_lock)
		mutex_unlock(&binder_lock);
	if (buf > page + PAGE_SIZE)
		buf = page + PAGE_SIZE;

	*start = page + off;

	len = buf - page;
	if (len > off)
		len -= off;
	else
		len = 0;

	return len < count ? len  : count;
}

static int binder_read_proc_stats(
	char *page, char **start, off_t off, int count, int *eof, void *data)
{
	struct binder_proc *proc;
	struct hlist_node *pos;
	int len = 0;
	char *p = page;
	int do_lock = !binder_debug_no_lock;

	if (off)
		return 0;

	if (do_lock)
		mutex_lock(&binder_lock);

	p += snprintf(p, PAGE_SIZE, "binder stats:\n");

	p = print_binder_stats(p, page + PAGE_SIZE, "", &binder_stats);

	hlist_for_each_entry(proc, pos, &binder_procs, proc_node) {
		if (p >= page + PAGE_SIZE)
			break;
		p = print_binder_proc_stats(p, page + PAGE_SIZE, proc);
	}
	if (do_lock)
		mutex_unlock(&binder_lock);
	if (p > page + PAGE_SIZE)
		p = page + PAGE_SIZE;

	*start = page + off;

	len = p - page;
	if (len > off)
		len -= off;
	else
		len = 0;

	return len < count ? len  : count;
}

static int binder_read_proc_transactions(
	char *page, char **start, off_t off, int count, int *eof, void *data)
{
	struct binder_proc *proc;
	struct hlist_node *pos;
	int len = 0;
	char *buf = page;
	char *end = page + PAGE_SIZE;
	int do_lock = !binder_debug_no_lock;

	if (off)
		return 0;

	if (do_lock)
		mutex_lock(&binder_lock);

	buf += snprintf(buf, end - buf, "binder transactions:\n");
	hlist_for_each_entry(proc, pos, &binder_procs, proc_node) {
		if (buf >= end)
			break;
		buf = print_binder_proc(buf, end, proc, 0);
	}
	if (do_lock)
		mutex_unlock(&binder_lock);
	if (buf > page + PAGE_SIZE)
		buf = page + PAGE_SIZE;

	*start = page + off;

	len = buf - page;
	if (len > off)
		len -= off;
	else
		len = 0;

	return len < count ? len  : count;
}

static int binder_read_proc_proc(
	char *page, char **start, off_t off, int count, int *eof, void *data)
{
	struct binder_proc *proc = data;
	int len = 0;
	char *p = page;
	int do_lock = !binder_debug_no_lock;

	if (off)
		return 0;

	if (do_lock)
		mutex_lock(&binder_lock);
	p += snprintf(p, PAGE_SIZE, "binder proc state:\n");
	p = print_binder_proc(p, page + PAGE_SIZE, proc, 1);
	if (do_lock)
		mutex_unlock(&binder_lock);

	if (p > page + PAGE_SIZE)
		p = page + PAGE_SIZE;
	*start = page + off;

	len = p - page;
	if (len > off)
		len -= off;
	else
		len = 0;

	return len < count ? len  : count;
}

static char *print_binder_transaction_log_entry(char *buf, char *end, struct binder_transaction_log_entry *e)
{
	buf += snprintf(buf, end - buf, "%d: %s from %d:%d to %d:%d node %d handle %d size %d:%d\n",
			e->debug_id, (e->call_type == 2) ? "reply" :
			((e->call_type == 1) ? "async" : "call "), e->from_proc,
			e->from_thread, e->to_proc, e->to_thread, e->to_node,
			e->target_handle, e->data_size, e->offsets_size);
	return buf;
}

static int binder_read_proc_transaction_log(
	char *page, char **start, off_t off, int count, int *eof, void *data)
{
	struct binder_transaction_log *log = data;
	int len = 0;
	int i;
	char *buf = page;
	char *end = page + PAGE_SIZE;

	if (off)
		return 0;

	if (log->full) {
		for (i = log->next; i < ARRAY_SIZE(log->entry); i++) {
			if (buf >= end)
				break;
			buf = print_binder_transaction_log_entry(buf, end, &log->entry[i]);
		}
	}
	for (i = 0; i < log->next; i++) {
		if (buf >= end)
			break;
		buf = print_binder_transaction_log_entry(buf, end, &log->entry[i]);
	}

	*start = page + off;

	len = buf - page;
	if (len > off)
		len -= off;
	else
		len = 0;

	return len < count ? len  : count;
}

static struct file_operations binder_fops = {
	.owner = THIS_MODULE,
	.poll = binder_poll,
	.unlocked_ioctl = binder_ioctl,
	.mmap = binder_mmap,
	.open = binder_open,
	.flush = binder_flush,
	.release = binder_release,
};

static struct miscdevice binder_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "binder",
	.fops = &binder_fops
};

static int __init binder_init(void)
{
	int ret;

	binder_proc_dir_entry_root = proc_mkdir("binder", NULL);
	if (binder_proc_dir_entry_root)
		binder_proc_dir_entry_proc = proc_mkdir("proc", binder_proc_dir_entry_root);
	ret = misc_register(&binder_miscdev);
	if (binder_proc_dir_entry_root) {
		create_proc_read_entry("state", S_IRUGO, binder_proc_dir_entry_root, binder_read_proc_state, NULL);
		create_proc_read_entry("stats", S_IRUGO, binder_proc_dir_entry_root, binder_read_proc_stats, NULL);
		create_proc_read_entry("transactions", S_IRUGO, binder_proc_dir_entry_root, binder_read_proc_transactions, NULL);
		create_proc_read_entry("transaction_log", S_IRUGO, binder_proc_dir_entry_root, binder_read_proc_transaction_log, &binder_transaction_log);
		create_proc_read_entry("failed_transaction_log", S_IRUGO, binder_proc_dir_entry_root, binder_read_proc_transaction_log, &binder_transaction_log_failed);
	}
	return ret;
}

device_initcall(binder_init);

MODULE_LICENSE("GPL v2");
