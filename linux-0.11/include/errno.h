#ifndef _ERRNO_H
#define _ERRNO_H

/*
 * ok, as I hadn't got any other source of information about
 * possible error numbers, I was forced to use the same numbers
 * as minix.
 * Hopefully these are posix or something. I wouldn't know (and posix
 * isn't telling me - they want $$$ for their f***ing standard).
 *
 * We don't use the _SIGN cludge of minix, so kernel returns must
 * see to the sign by themselves. 没有使用minix那样的_SIGN簇, 所以内核的返回值必须自己辨别正负号
 *如果改变该文件的话, 记着也要修改 strerror()
 * NOTE! Remember to change strerror() if you change this file!
 */
//系统调用以及很多库函数返回一个特殊的值以表示操作失败或出错. 这个值通常选择-1或其他一些特定的值来表示, 但是这个返回值仅说明错误发生了
// 如果需要知道出错的类型,就需要查看表示系统出错号的变量errno. 在程序开始执行时该变量被初始化为0
extern int errno;
//在出错时, 系统调用会把出错号放在变量errno中(负值), 然后返回-1. 因此程序若需要知道具体错误号,就需要查看errno的值
#define ERROR		99 //一般错误
#define EPERM		 1 //操作没有许可
#define ENOENT		 2 //文件或目录不存在
#define ESRCH		 3 //指定的进程不存在
#define EINTR		 4 //中断的系统调用
#define EIO		 5 //输入输出错
#define ENXIO		 6  // 指定设备或地址不存在
#define E2BIG		 7  //参数列表太长
#define ENOEXEC		 8  // 执行程序格式错误
#define EBADF		 9 // 文件句柄(描述符)错误
#define ECHILD		10  // 子进程不存在
#define EAGAIN		11  // 资源暂时不可用
#define ENOMEM		12 // 内存不足
#define EACCES		13 //没有许可权限
#define EFAULT		14 //地址错
#define ENOTBLK		15  //不是块设备文件
#define EBUSY		16  //资源正忙
#define EEXIST		17 //文件已存在
#define EXDEV		18 // 非法连接
#define ENODEV		19 // 设备不存在
#define ENOTDIR		20  // 不是目录文件
#define EISDIR		21 // 是目录文件
#define EINVAL		22  // 参数无效
#define ENFILE		23 //系统打开文件数太多
#define EMFILE		24 // 打开文件数太多
#define ENOTTY		25 // 不恰当的IO控制操作(没有tty终端)
#define ETXTBSY		26 // 不再使用
#define EFBIG		27 //文件太大
#define ENOSPC		28 //设备已满(设备已经没有空间)
#define ESPIPE		29 // 无效的文件指针重定位
#define EROFS		30 // 文件系统只读
#define EMLINK		31 // 连接太多
#define EPIPE		32 //管道错
#define EDOM		33 // 域出错
#define ERANGE		34 // 结果太大
#define EDEADLK		35 // 避免死锁资源
#define ENAMETOOLONG	36 // 文件名太长
#define ENOLCK		37 // 没有锁定可用
#define ENOSYS		38 //功能还没有实现
#define ENOTEMPTY	39 // 目录不空

#endif
