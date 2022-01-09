/*
 * Copyright (C) 2008 Google, Inc.
 *
 * Based on, but no longer compatible with, the original
 * OpenBinder.org binder driver interface, which is:
 *
 * Copyright (c) 2005 Palmsource, Inc.
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

#ifndef _LINUX_BINDER_H
#define _LINUX_BINDER_H

#include <linux/ioctl.h>

#define B_PACK_CHARS(c1, c2, c3, c4) \
	((((c1)<<24)) | (((c2)<<16)) | (((c3)<<8)) | (c4))
#define B_TYPE_LARGE 0x85

enum {
	BINDER_TYPE_BINDER	= B_PACK_CHARS('s', 'b', '*', B_TYPE_LARGE),
	BINDER_TYPE_WEAK_BINDER	= B_PACK_CHARS('w', 'b', '*', B_TYPE_LARGE),
	BINDER_TYPE_HANDLE	= B_PACK_CHARS('s', 'h', '*', B_TYPE_LARGE),
	BINDER_TYPE_WEAK_HANDLE	= B_PACK_CHARS('w', 'h', '*', B_TYPE_LARGE),
	BINDER_TYPE_FD		= B_PACK_CHARS('f', 'd', '*', B_TYPE_LARGE),
};

enum {
	FLAT_BINDER_FLAG_PRIORITY_MASK = 0xff,
	FLAT_BINDER_FLAG_ACCEPTS_FDS = 0x100,
};

/*
 * This is the flattened representation of a Binder object for transfer
 * between processes.  The 'offsets' supplied as part of a binder transaction
 * contains offsets into the data where these structures occur.  The Binder
 * driver takes care of re-writing the structure type and data as it moves
 * between processes.
 */
struct flat_binder_object {
	/* 8 bytes for large_flat_header. */
	unsigned long		type;
	unsigned long		flags;

	/* 8 bytes of data. */
	union {
		void		*binder;	/* local object */
		signed long	handle;		/* remote object */
	};

	/* extra data associated with local object */
	void			*cookie;
};

/*
 * On 64-bit platforms where user code may run in 32-bits the driver must
 * translate the buffer (and local binder) addresses apropriately.
 */

/**
 * @brief binder_write_read用来描述进程间通信过程中所传输的数据。这些数据包括输入数据和
 * 输出数据，其中，成员变量write_size、write_consurmed和write_buffer用来描述输入数据，
 * 即从用户空间传输到Binder驱动程序的数据；而成员变量read_size、read_consumed和read_buffer
 * 用来描述输出数据，即从Binder驱动程序返回给用户空间的数据，他也是进程间通信结果数据。
 * 
 * 缓冲区write_buffer和read_buffer的数据格式如下
 * ----------------------------------------------------------------------------------
 * Code-1|Content of Code-1|Code-2|Content of Code-2|......|Code-N|Content of Code-N|
 * -----------------------------------------------------------------------------------
 * 
 * 他们都是一个数组，数组的每一个元素都由一个通信协议代码及其通信数据组成。
 * 协议代码分为两种类型：	1.在输入缓冲区write_buffer中使用的，称为命令协议代码。
 * 						通过BinderDriverCommandProtcol枚举来定义。
 * 						2.在输出缓冲区read_buffer中使用的，称为返回协议代码	
 * 						通过BinderDriverReturnCommandProtocol枚举来定义。	
 */
struct binder_write_read {
	//指向一个用户空间缓冲区的地址，里面保存的内容即为要传输到Binder驱动程序的数据。
	unsigned long	write_buffer;
	//缓冲区write_buffer的大小，单位是字节
	signed long	write_size;	/* bytes to write */
	//表述Binder驱动程序从缓冲区write_buffer中处理了多少个字节的数据
	signed long	write_consumed;	/* bytes consumed by driver */

	//指向一个用户空间缓冲区的地址，里面保存的内容即为Binder驱动程序返回给用户空间的进程间
	//通信结果数据。
	unsigned long	read_buffer;
	//缓冲区read_buffer的大小，单位是字节
	signed long	read_size;	/* bytes to read */
	//用来描述用户空间应用程序从缓冲区read_buffer中处理了多少个字节的数据
	signed long	read_consumed;	/* bytes consumed by driver */
};

/* Use with BINDER_VERSION, driver fills in fields. */
struct binder_version {
	/* driver protocol version -- increment with incompatible change */
	signed long	protocol_version;
};

/* This is the current protocol version. */
#define BINDER_CURRENT_PROTOCOL_VERSION 7

/**
 * @brief 应用程序进程在打开了设备文件/dev/binder之后，需要通过IO控制函数ioctl来进一步于Binder
 * 驱动程序进行交互，因此，Binder驱动程序就提供了一系列的IO控制命令来和应用程序进行通信。在这些IO
 * 控制命令中，最重要的便是BINDER_WRITE_READ了
 * 
 */
#define BINDER_WRITE_READ   		_IOWR('b', 1, struct binder_write_read)
#define	BINDER_SET_IDLE_TIMEOUT		_IOW('b', 3, int64_t)
#define	BINDER_SET_MAX_THREADS		_IOW('b', 5, size_t)
#define	BINDER_SET_IDLE_PRIORITY	_IOW('b', 6, int)
#define	BINDER_SET_CONTEXT_MGR		_IOW('b', 7, int)
#define	BINDER_THREAD_EXIT		_IOW('b', 8, int)
#define BINDER_VERSION			_IOWR('b', 9, struct binder_version)

/*
 * NOTE: Two special error codes you should check for when calling
 * in to the driver are:
 *
 * EINTR -- The operation has been interupted.  This should be
 * handled by retrying the ioctl() until a different error code
 * is returned.
 *
 * ECONNREFUSED -- The driver is no longer accepting operations
 * from your process.  That is, the process is being destroyed.
 * You should handle this by exiting from your process.  Note
 * that once this error code is returned, all further calls to
 * the driver from any thread will return this same code.
 */

enum transaction_flags {
	TF_ONE_WAY	= 0x01,	/* this is a one-way call: async, no return */
	TF_ROOT_OBJECT	= 0x04,	/* contents are the component's root object */
	TF_STATUS_CODE	= 0x08,	/* contents are a 32-bit status code */
	TF_ACCEPT_FDS	= 0x10,	/* allow replies with file descriptors */
};

struct binder_transaction_data {
	/* The first two are only used for bcTRANSACTION and brTRANSACTION,
	 * identifying the target and contents of the transaction.
	 */
	union {
		size_t	handle;	/* target descriptor of command transaction */
		void	*ptr;	/* target descriptor of return transaction */
	} target;
	void		*cookie;	/* target object cookie */
	unsigned int	code;		/* transaction command */

	/* General information about the transaction. */
	unsigned int	flags;
	pid_t		sender_pid;
	uid_t		sender_euid;
	size_t		data_size;	/* number of bytes of data */
	size_t		offsets_size;	/* number of bytes of offsets */

	/* If this transaction is inline, the data immediately
	 * follows here; otherwise, it ends with a pointer to
	 * the data buffer.
	 */
	union {
		struct {
			/* transaction data */
			const void	*buffer;
			/* offsets from buffer to flat_binder_object structs */
			const void	*offsets;
		} ptr;
		uint8_t	buf[8];
	} data;
};

struct binder_ptr_cookie {
	void *ptr;
	void *cookie;
};

struct binder_pri_desc {
	int priority;
	int desc;
};

struct binder_pri_ptr_cookie {
	int priority;
	void *ptr;
	void *cookie;
};

/**
 * @brief 定义返回协议代码的类型
 * 
 */
enum BinderDriverReturnProtocol {
	//返回协议代码BR_ERROR后面跟的通信数据是一个整数，用来描述一个错误代码。
	//Binder驱动程序在处理应用程序进程发出来的某个请求时，如果发生了异常情况，他就会使用
	//返回协议代码BR_ERROR来通知应用程序进程。
	BR_ERROR = _IOR('r', 0, int),
	/*
	 * int: error code
	 */

	//BR_OK后面不需要制定通信数据。Binder驱动程序成功处理了应用程序发出的某个请求之后，
	//他就会使用返回协议代码BR_OK来通知应用程序进程。
	BR_OK = _IO('r', 1),
	/* No parameters! */

	//BR_TRANSACTION和BR_REPLY后面跟的通信数据使用一个结构体binder_transaction_data来描述。
	//当一个Client进程向一个Server进程发出进程间通信请求时，Binder驱动程序就会使用返回协议代码
	//BR_TRANSACTION通知该Server进程来处理该进程间通信请求；当Server进程处理完成该进程间通信请求
	//之后，Binder驱动程序就会使用返回协议代码BR_REPLY将进程间通信请求结果数据返回给Client进程。
	BR_TRANSACTION = _IOR('r', 2, struct binder_transaction_data),
	BR_REPLY = _IOR('r', 3, struct binder_transaction_data),
	/*
	 * binder_transaction_data: the received command.
	 */

	//当前Binder驱动程序实现中不支持
	BR_ACQUIRE_RESULT = _IOR('r', 4, int),
	/*
	 * not currently supported
	 * int: 0 if the last bcATTEMPT_ACQUIRE was not successful.
	 * Else the remote object has acquired a primary reference.
	 */

	//返回协议代码BR_DEAD_REPLY后面不需要制定通信数据。Bidner驱动程序在处理进程间通信请求时，
	//如果发现目标进程或者目标线程已经死亡，他就会使用返回协议代码BR_DEAD_REPLY来通知源进程。
	BR_DEAD_REPLY = _IO('r', 5),
	/*
	 * The target of the last transaction (either a bcTRANSACTION or
	 * a bcATTEMPT_ACQUIRE) is no longer with us.  No parameters.
	 */

	//返回协议代码BR_TRANSACTION_COMPLETE后面不需要制定通信数据。当Binder驱动程序接收到应用程序
	//进程给他发送的一个命令协议代码BC_TRANSACTION或者BC_REPLY时，他就会使用返回协议代码
	//BR_TRANSACTION_COMPLETE来通知应用程序，该命令协议代码已被接收，正在分发给目标进程或者
	//目标线程处理。
	BR_TRANSACTION_COMPLETE = _IO('r', 6),
	/*
	 * No parameters... always refers to the last transaction requested
	 * (including replies).  Note that this will be sent even for
	 * asynchronous transactions.
	 */

	//BR_INCREFS、BR_ACQUIRE、BR_RELEASE和BR_DECREFS后面跟的通信数据使用一个结构体
	//binder_ptr_cookie来描述

	//增加一个Service组件的弱引用计数
	BR_INCREFS = _IOR('r', 7, struct binder_ptr_cookie),
	//减少一个Service组件的弱引用计数
	BR_DECREFS = _IOR('r', 10, struct binder_ptr_cookie),
	
	//增加一个Service组件的强引用计数
	BR_ACQUIRE = _IOR('r', 8, struct binder_ptr_cookie),
	//减少一个Service组件的弱引用计数
	BR_RELEASE = _IOR('r', 9, struct binder_ptr_cookie),
	
	/*
	 * void *:	ptr to binder
	 * void *: cookie for binder
	 */

	//当前的Binder驱动程序实现中不支持
	BR_ATTEMPT_ACQUIRE = _IOR('r', 11, struct binder_pri_ptr_cookie),
	/*
	 * not currently supported
	 * int:	priority
	 * void *: ptr to binder
	 * void *: cookie for binder
	 */

	//返回协议代码BR_NOOP后面不需要指定通信数据。Binder驱动程序使用返回协议代码BR_NOOP
	//来通知应用程序进程执行一个空操作，他的存在是为了方便以后可以替换返回协议代码BR_SPAWN_LOOPER。
	BR_NOOP = _IO('r', 12),
	/*
	 * No parameters.  Do nothing and examine the next command.  It exists
	 * primarily so that we can replace it with a BR_SPAWN_LOOPER command.
	 */

	//返回协议代码BR_SPAWN_LOOPER后面不需要指定通信数据。当Binder驱动程序发现一个进程
	//没有足够的空闲Binder线程来处理进程间通信请求时，他就会使用返回协议代码BR_SPAWN_LOOP
	//来通知该进程增加一个新的线程到Binder线程池中。
	//spawn：产卵
	BR_SPAWN_LOOPER = _IO('r', 13),
	/*
	 * No parameters.  The driver has determined that a process has no
	 * threads waiting to service incomming transactions.  When a process
	 * receives this command, it must spawn a new service thread and
	 * register it via bcENTER_LOOPER.
	 */

	//当前Binder驱动程序实现中不支持
	BR_FINISHED = _IO('r', 14),
	/*
	 * not currently supported
	 * stop threadpool thread
	 */

	//BR_DEAD_BINDER和BR_CLEAR_DEATH_NOTIFICATION_DONE后面跟的通信数据是一个void类型的指针，
	//他指向一个用来接收Service组件死亡通知的对象的地址。当Binder驱动程序监测到一个Service组件的
	//死亡事件时，他就会使用返回协议代码BR_DEAD_BINDER来通知相应的Client进程。当Client进程通知Binder
	//驱动程序注销他之前所注册的一个死亡接收通知时，Binder驱动程序执行完成之歌注销操作之后，就会使用
	//BR_CLEAR_DEATH_NOTIFICATION_DONE来通知Cient进程。
	BR_DEAD_BINDER = _IOR('r', 15, void *),
	/*
	 * void *: cookie
	 */
	BR_CLEAR_DEATH_NOTIFICATION_DONE = _IOR('r', 16, void *),
	/*
	 * void *: cookie
	 */

	//返回协议BR_FAILED_REPLY后面不需要指定通信数据。当Binder驱动程序处理一个进程发出的BC_TRANSACTION命令协议时
	//，如果发生了异常情况，他就会使用返回协议代码BR_FAILED_REPLY来通知源进程。
	BR_FAILED_REPLY = _IO('r', 17),
	/*
	 * The the last transaction (either a bcTRANSACTION or
	 * a bcATTEMPT_ACQUIRE) failed (e.g. out of memory).  No parameters.
	 */
};

/**
 * @brief 定义命令协议代码类型
 * 
 */
enum BinderDriverCommandProtocol {
	//BC_TRANSACTION和BC_REPLY后面跟的通信数据使用一个结构体binder_transaction_data来描述。

	//当一个进程请求另外一个进程执行某一个操作时，源进程就是使用命令协议代码BC_TRANSACTION来请求Binder驱动程序
	//将通信数据传递到目标进程
	BC_TRANSACTION = _IOW('c', 0, struct binder_transaction_data),
	//当目标进程处理完成源进程所请求的操作之后，他就是用命令协议BC_REPLY来请求Binder驱动程序将结果数据传递给源进程。
	BC_REPLY = _IOW('c', 1, struct binder_transaction_data),
	/*
	 * binder_transaction_data: the sent command.
	 */

	//当前的Binder驱动程序实现中不支持
	BC_ACQUIRE_RESULT = _IOW('c', 2, int),
	/*
	 * not currently supported
	 * int:  0 if the last BR_ATTEMPT_ACQUIRE was not successful.
	 * Else you have acquired a primary reference on the object.
	 */

	//命令协议代码BC_FREE_BUFFER后面跟的通信数据是一个整数，他指向了在Binder驱动程序内部所分配的一块内核缓冲区。
	//Binder驱动程序就是通过这个内核缓冲区将源进程的通信数据传递到目标进程的。当目标进程处理完成源进程的通信请求后，
	//他就会使用命令协议代码BC_FREE_BUFFER来通知Binder驱动程序释放这个内核缓冲区。
	BC_FREE_BUFFER = _IOW('c', 3, int),
	/*
	 * void *: ptr to transaction data received on a read
	 */

	//命令协议BC_INCREFS、BC_ACQUIRE、BC_RELEASE和BC_DECREFS后面跟着的通信数据是一个整数，它描述了一个Binder
	//引用对象的句柄

	//增加一个Binder引用对象的弱引用计数值
	BC_INCREFS = _IOW('c', 4, int),
	//减少一个Binder引用对象的弱引用计数值
	BC_DECREFS = _IOW('c', 7, int),

	//增加一个Binder引用对象的强引用计数值
	BC_ACQUIRE = _IOW('c', 5, int),
	//减少一个Binder引用对象的弱引用计数值
	BC_RELEASE = _IOW('c', 6, int),
	
	/*
	 * int:	descriptor
	 */
	//Binder驱动程序第一次增加一个Binder实体对象的强引用计数或者弱引用计数时，就会使用返回协议代码
	//BR_ACQUIRE或者BR_INCREFS来请求对应的Server进程增加对应的Service组件的强引用计数或者弱引用计数。
	//当Server进程处理完成这两个请求之后，就会分别使用命令协议代码BC_INCREFS_DONE和BC_ACQUIRE_DONE
	//将操作结果返回给Binder驱动程序
	//

	BC_INCREFS_DONE = _IOW('c', 8, struct binder_ptr_cookie),
	BC_ACQUIRE_DONE = _IOW('c', 9, struct binder_ptr_cookie),
	/*
	 * void *: ptr to binder
	 * void *: cookie for binder
	 */

	//当前Binder驱动程序实现中不支持
	BC_ATTEMPT_ACQUIRE = _IOW('c', 10, struct binder_pri_desc),
	/*
	 * not currently supported
	 * int: priority
	 * int: descriptor
	 */

	//命令协议BC_REGISTER_LOOPER、BC_ENTER_LOOPER和BC_EXIT_LOOPER后面不需要制定通信数据。
	//一方面，当一个线程将自己注册到Binder驱动程序之后，他接着就会使用命令协议代码BC_ENTER_LOOPER来通知
	//Binder驱动程序，他已经准备就绪处理进程间通信请求了；另一方面，当Binder驱动程序主动请求进程
	//注册一个新的线程到他的Binder线程池中来处理进程间通信请求之后，新创建的线程就会使用命令协议代码
	//BC_REGISTER_LOOPER来通知Binder驱动程序，他准备就绪了。
	//最后，当一个线程要退出时，他就是用命令协议代码BC_EXIT_LOOPER从Binder驱动程序中注销，这样他就不会
	//再收到进程间请求通信了。

	BC_REGISTER_LOOPER = _IO('c', 11),
	/*
	 * No parameters.
	 * Register a spawned looper thread with the device.
	 */

	BC_ENTER_LOOPER = _IO('c', 12),
	BC_EXIT_LOOPER = _IO('c', 13),
	/*
	 * No parameters.
	 * These two commands are sent as an application-level thread
	 * enters and exits the binder loop, respectively.  They are
	 * used so the binder can have an accurate count of the number
	 * of looping threads it has available.
	 */

	//BC_REQUEST_DEATH_NOTIFICATION和BC_CLEAR_DEATH_NOTIFICATION后面跟的通信数据
	//使用一个结构体binder_ptr_cookie来描述。
	//一方面，如果一个进程希望获得他所引用的Service组件的死亡接收通知，
	//那么他就需要使用命令协议BC_REQUEST_DEATH_NOTIFICATION来向Binder驱动程序注册一个死亡通知；
	//另一方面，如果一个进程想注销之前所注册的一个死亡接收通知，那么他就需要使用命令协议
	//BC_CLEAR_DEATH_NOTIFICATION来向Binder驱动程序发出请求。

	BC_REQUEST_DEATH_NOTIFICATION = _IOW('c', 14, struct binder_ptr_cookie),
	/*
	 * void *: ptr to binder
	 * void *: cookie
	 */

	BC_CLEAR_DEATH_NOTIFICATION = _IOW('c', 15, struct binder_ptr_cookie),
	/*
	 * void *: ptr to binder
	 * void *: cookie
	 */

	//命令协议代码BC_DEAD_BINDER_DONE后面跟的通信数据是一个void类型的指针，指向一个死亡接收通知
	//结构体binder_ref_death的地址。当一个进程获得一个Service组件的死亡通知时，他就会使用命令协议
	//代码BC_DEAD_BINDER_DONE来通知Binder驱动程序，他已经处理完成该Servcie组件的死亡通知了。
	BC_DEAD_BINDER_DONE = _IOW('c', 16, void *),
	/*
	 * void *: cookie
	 */
};

#endif /* _LINUX_BINDER_H */

