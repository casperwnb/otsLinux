/*
 *  linux/fs/char_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <sys/types.h>

#include <linux/sched.h>
#include <linux/kernel.h>

#include <asm/segment.h>
#include <asm/io.h>

extern int tty_read(unsigned minor,char * buf,int count); //终端读
extern int tty_write(unsigned minor,char * buf,int count); //终端写
//定义字符设备读写函数指针类型
typedef (*crw_ptr)(int rw,unsigned minor,char * buf,int count,off_t * pos);
//串口终端读写操作函数; rw读写命令; minor:终端子设备号; buf 缓冲区; count: 读写字节数
//pos 读写操作当前指针,对于终端操作,该指针无用; 返回实际读写的字节数,若失败则返回出错码
static int rw_ttyx(int rw,unsigned minor,char * buf,int count,off_t * pos)
{
	return ((rw==READ)?tty_read(minor,buf,count):
		tty_write(minor,buf,count));
}
//终端读写操作, 同rw_ttyx(), 只是增加了对进程是否有控制终端的检测
static int rw_tty(int rw,unsigned minor,char * buf,int count, off_t * pos)
{
	if (current->tty<0) //若进程没有对应的控制终端,则返回出错码.否则调用终端读写函数
		return -EPERM;  //rw_ttyx(), 并返回实际读写字节数
	return rw_ttyx(rw,current->tty,buf,count,pos);
}
//内存数据读写,未实现
static int rw_ram(int rw,char * buf, int count, off_t *pos)
{
	return -EIO;
}
//物理内存数据读写操作函数,未实现
static int rw_mem(int rw,char * buf, int count, off_t * pos)
{
	return -EIO;
}
//内核虚拟内存数据读写函数,未实现
static int rw_kmem(int rw,char * buf, int count, off_t * pos)
{
	return -EIO;
}
//端口读写操作函数. rw读写命令, buf缓冲区; count 读写字节数; pos端口地址; 返回实际读写的字节数
static int rw_port(int rw,char * buf, int count, off_t * pos)
{
	int i=*pos;
//对于所有要求读写的字节数,且端口地址 <64k时, 循环执行单个字节的读写操作. 若是读命令,则从
//端口i中读取一字节内容并放到用户缓冲区中, 若是写命令, 则从用户数据缓冲区中取一字节输出到
//端口i
	while (count-->0 && i<65536) {
		if (rw==READ)
			put_fs_byte(inb(i),buf++);
		else
			outb(get_fs_byte(buf++),i);
		i++; //前移一个端口
	}
	i -= *pos; //计算读写的字节数,调整相应读写指针,并返回读写的字节数
	*pos += i;
	return i;
}
//内存读写操作函数
static int rw_memory(int rw, unsigned minor, char * buf, int count, off_t * pos)
{
	switch(minor) { //根据内存设备子设备号, 分别调用不同的内存读写函数
		case 0:
			return rw_ram(rw,buf,count,pos);
		case 1:
			return rw_mem(rw,buf,count,pos);
		case 2:
			return rw_kmem(rw,buf,count,pos);
		case 3:
			return (rw==READ)?0:count;	/* rw_null */
		case 4:
			return rw_port(rw,buf,count,pos);
		default:
			return -EIO;
	}
}
//定义系统中设备种数
#define NRDEVS ((sizeof (crw_table))/(sizeof (crw_ptr)))
//字符设备读写函数指针表
static crw_ptr crw_table[]={
	NULL,		/* nodev */ //无设备(空设备)
	rw_memory,	/* /dev/mem etc */
	NULL,		/* /dev/fd */
	NULL,		/* /dev/hd */
	rw_ttyx,	/* /dev/ttyx */
	rw_tty,		/* /dev/tty */
	NULL,		/* /dev/lp */
	NULL};		/* unnamed pipes */
//字符设备读写操作函数; rw读写命令; dev设备号; buf缓冲区; count读写字节数; pos读写指针
//返回实际读/写字节数
int rw_char(int rw,int dev, char * buf, int count, off_t * pos)
{
	crw_ptr call_addr;
//如果设备号超出系统设备数,则返回出错码; 如果该设备没有对应的读/写函数, 也返回出错码. 否则
//调用对应设备的读写操作函数,返回实际读/写的字节数
	if (MAJOR(dev)>=NRDEVS)
		return -ENODEV;
	if (!(call_addr=crw_table[MAJOR(dev)]))
		return -ENODEV;
	return call_addr(rw,MINOR(dev),buf,count,pos);
}
