/*
 *  linux/kernel/hd.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This is the low-level hd interrupt support. It traverses the
 * request-list, using interrupts to jump between functions. As
 * all the functions are called within interrupts, we may not
 * sleep. Special care is recommended.
 * 由于所有的函数都是在中断里调用的,所以这些函数不可以睡眠
 *  modified by Drew Eckhardt to check nr of hd's from the CMOS.
 */

#include <linux/config.h>  // 内核配置头文件, 定义键盘语言和硬盘类型选项
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>  // 硬盘参数头文件. 定义硬盘寄存器端口、状态码、分区表等信息
#include <asm/system.h> // 系统头文件
#include <asm/io.h>  // io头文件
#include <asm/segment.h>  // 段操作头文件
//定义硬盘主设备号符号常数,驱动程序中主设备号必须在包含blk.h文件之前被定义,因为blk.h文件中要用到该符号常数值来确定一系列其他相关符号常数和宏
#define MAJOR_NR 3  // 硬盘主设备号=3
#include "blk.h"  // 块设备头文件
// 读CMOS参数宏函数, outb_p, inb_p是include/asm/io.h中定义的端口输入输出宏.与 init/main.c中读取cmos时钟信息的宏一样.
// 0x70是写端口, 0x80|addr 是要读的cmos内存地址. 0x71是读端口
#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

/* Max read/write errors/sector */  // 每扇区读、写操作允许的最多出错次数
#define MAX_ERRORS	7  // 读、写一个扇区时允许的最多出错次数
#define MAX_HD		2  // 系统支持的最多硬盘数
// 重新校正处理函数, 复位操作时在硬盘中断处理程序中调用的重新校正函数
static void recal_intr(void);
// 重新校正标志, 当设置了该标志,程序中会调用 recal_intr()以将磁头移动到0柱面
static int recalibrate = 1;
static int reset = 1;  // 复位标志,当发送读写错误时会设置该标志并调用相关复位函数,以复位硬盘和控制器

/*
 *  This struct defines the HD's and their types.
 */
// 定义了硬盘参数及类型, 硬盘信息结构(harddisk information struct), 各字段分别是磁头数、每磁道扇区数、柱面数、写前预补偿柱面号
// 磁头着陆区柱面号、控制字节
struct hd_i_struct {
	int head,sect,cyl,wpcom,lzone,ctl;
	};
#ifdef HD_TYPE // 如果已经在 include/linux/config.h配置文件总定义了符号常数HD_TYPE,就取其中定义号的参数作为硬盘信息数组hd_info
//中的数据,否则先默认都设置为0, 在 setup()函数中会重新进行设置.
struct hd_i_struct hd_info[] = { HD_TYPE };  // 硬盘信息数组
#define NR_HD ((sizeof (hd_info))/(sizeof (struct hd_i_struct)))  // 计算硬盘个数
#else
struct hd_i_struct hd_info[] = { {0,0,0,0,0,0},{0,0,0,0,0,0} };
static int NR_HD = 0;
#endif
// 定义硬盘分区结构, 给出每个分区从硬盘0道开始算起的物理起始扇区号和分区扇区总数, 其中5的倍数处项(如hd[0],hd[5])代表整个硬盘的参数
static struct hd_struct {
	long start_sect;  // 分区在硬盘中的起始物理(绝对)扇区
	long nr_sects;  // 分区中扇区总数
} hd[5*MAX_HD]={{0,0},};
// 读端口嵌入汇编. 读端口port, 共读nr字, 保存在buf中
#define port_read(port,buf,nr) \
__asm__("cld;rep;insw"::"d" (port),"D" (buf),"c" (nr):"cx","di")
//写端口嵌入汇编表,写端口port, 共写nr字, 从buf中取数据
#define port_write(port,buf,nr) \
__asm__("cld;rep;outsw"::"d" (port),"S" (buf),"c" (nr):"cx","si")

extern void hd_interrupt(void); // 硬盘中断过程
extern void rd_load(void);  // 虚拟盘创建加载函数

/* This may be used only once, enforced by 'static int callable' */
// 该函数只在初始化时被调用一次, 用静态变量callable作为可调用标志. 系统设置函数, 函数参数BIOS是由初始化程序 init/main.c中init子程序
//设置为指向硬盘参事室表结构的指针. 该硬盘参数表结构包含2个硬盘参数表的内容(共32字节), 是从内存0x90080处复制而来. 0x90080处的硬盘参数表
//是由setup.s程序利用BIOS功能取得. 本函数主要功能是读取CMOS和硬盘参数表信息, 用于设置硬盘分区结构hd, 并尝试加载RAM虚拟盘和根文件系统
int sys_setup(void * BIOS)
{
	static int callable = 1;  // 限制本函数只能被调用1次的标志
	int i,drive;
	unsigned char cmos_disks;
	struct partition *p;
	struct buffer_head * bh;
// 首先设置callable标志,使得本函数只能被调用1次, 然后设置硬盘信息数组hd_info[], 如果在 include/linux/config.h中已定义了符号常数
//HD_TYPE,那么hd_info[]数组已经在前面设置好了,否则就需要读取boot/setup.s程序放在0x90080处开始的硬盘参数表. setup.s程序在内存此处
//连续存放着一到两个硬盘参数表
	if (!callable)
		return -1;
	callable = 0;
#ifndef HD_TYPE  // 没有定义HD_TYPE, 则读取
	for (drive=0 ; drive<2 ; drive++) {
		hd_info[drive].cyl = *(unsigned short *) BIOS;  // 柱面数
		hd_info[drive].head = *(unsigned char *) (2+BIOS);  // 磁头数
		hd_info[drive].wpcom = *(unsigned short *) (5+BIOS); // 写前预补偿柱面号
		hd_info[drive].ctl = *(unsigned char *) (8+BIOS);  // 控制字节
		hd_info[drive].lzone = *(unsigned short *) (12+BIOS);  // 磁头着陆区柱面号
		hd_info[drive].sect = *(unsigned char *) (14+BIOS);  // 每磁道扇区数
		BIOS += 16;  // 每个硬盘参数表长16字节, 这里BIOS指向下一表
	}
// setup.s 在取BIOS硬盘参数时, 如果系统中只有一个硬盘,就将对应第2个硬盘的16字节全部清零,因此这里只需要判断第2个硬盘柱面数是否为0就可以
//知道是否有第2个硬盘了
	if (hd_info[1].cyl)
		NR_HD=2;  // 硬盘数=2
	else
		NR_HD=1;
#endif
// 硬盘信息数组hd_info[]已经设置好了,且确定了系统含有的硬盘数NR_HD. 开始设置硬盘分区结构数组hd[],该数组的项0,5分别表示两个硬盘的整体
//参数,而项1-4,6-9分别表示两个硬盘的4个分区的参数. 因此这里仅设置表示硬盘整体信息的两项
	for (i=0 ; i<NR_HD ; i++) {
		hd[i*5].start_sect = 0;  // 硬盘起始扇区号
		hd[i*5].nr_sects = hd_info[i].head*
				hd_info[i].sect*hd_info[i].cyl;  // 硬盘总扇区数
	}

	/*
		We querry CMOS about hard disks : it could be that 
		we have a SCSI/ESDI/etc controller that is BIOS
		compatable with ST-506, and thus showing up in our
		BIOS table, but not register compatable, and therefore
		not present in CMOS.

		Furthurmore, we will assume that our ST-506 drives
		<if any> are the primary drives in the system, and 
		the ones reflected as drive 1 or 2.

		The first drive is stored in the high nibble of CMOS
		byte 0x12, the second in the low nibble.  This will be
		either a 4 bit drive type or 0xf indicating use byte 0x19 
		for an 8 bit type, drive 1, 0x1a for drive 2 in CMOS.

		Needless to say, a non-zero value means we have 
		an AT controller hard disk for that drive.
可能会出现这样的情况: 有一块scsi/esdi等的控制器,它是以St-506方式与bios相兼容,因而会出现在BIOS参数表中,但却不是寄存器兼容的,因此
这些参数在cmos中又不存在. 另外假设st-506驱动器(如果有的话)是系统中的基本驱动器,即以驱动器1或2出现的驱动器. 第1个驱动器参数存放在CMOS
字节0x12的高半字节中,第2个存放在低半字节中. 该4位字节信息可以是驱动器类型,也可能仅是0xf. 0xf表示使用CMOS中0x19字节作为驱动器1的
8位类型字节,使用CMOS中0x1a字节作为驱动器2的类型字节. 总之一个非0值意味着硬盘是一个at控制器兼容硬盘.
	*/
//下面代码用来检测硬盘到底是不是at控制器兼容的. 这里从cmos偏移地址0x12处读出硬盘类型字节. 如果低半字节值(存放着第2个硬盘类型值)不为0
//则表示系统有两个硬盘,否则表示系统只有1个硬盘,如果0x12处读出的值为0,则表示系统中没有at兼容硬盘
	if ((cmos_disks = CMOS_READ(0x12)) & 0xf0)
		if (cmos_disks & 0x0f)
			NR_HD = 2;
		else
			NR_HD = 1;
	else
		NR_HD = 0;
// 若NR_HD=0, 则两个硬盘都不是AT控制器兼容的, 两个硬盘数据结构全清零. NR_HD=1,则将第2个硬盘的参数清零
	for (i = NR_HD ; i < 2 ; i++) {
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = 0;
	}
//到此为止真正确定了系统中所含的硬盘个数NR_HD. 现在来读取每个硬盘上第一个扇区中的分区表信息, 用来设置分区结构数组 hd[]中硬盘各分区的信息
//首先利用读块函数bread()读硬盘第1个数据库,第1个参数0x300,0x305 分别是两个硬盘的设备号,第2个参数是所需读取的块号. 若读操作成功,则数据
//会被存放在缓冲块bh的数据区中. 若缓冲块头指针bh=0,则说明读操作失败,则显示出错信息并停机. 否则根据硬盘第1个扇区最后两个字节应该是0xAA55
//来判断扇区中数据的有效性,从而可以知道扇区中位于偏移0x1be开始处的分区表是否有效. 若有效则将硬盘分区表信息放入硬盘分区结构数组hd[]中,
//最后释放 bh 缓冲区
	for (drive=0 ; drive<NR_HD ; drive++) {
		if (!(bh = bread(0x300 + drive*5,0))) {  // 0x300, 0x305是设备号
			printk("Unable to read partition table of drive %d\n\r",
				drive);
			panic("");
		}
		if (bh->b_data[510] != 0x55 || (unsigned char)
		    bh->b_data[511] != 0xAA) {  // 判断硬盘标志 0xaa55
			printk("Bad partition table on drive %d\n\r",drive);
			panic("");
		}
		p = 0x1BE + (void *)bh->b_data;  // 分区表位于第1扇区0x1be处
		for (i=1;i<5;i++,p++) {
			hd[i+5*drive].start_sect = p->start_sect;
			hd[i+5*drive].nr_sects = p->nr_sects;
		}
		brelse(bh);  // 释放为存放硬盘数据块而申请的缓冲区
	}
//现在完成了设置硬盘分区结构数组 hd[]的任务, 如果确实有硬盘存在并且已读入其分区表,则显示"分区表正常"信息. 然后尝试在系统内存虚拟盘中
//加载启动盘中包含的根文件系统映像,即在系统设置有虚拟盘的情况下判断启动盘上是否还含有根文件系统的映像数据. 如果有(此时该启动盘称为集成盘)
//则尝试把该映像加载并存放到虚拟盘中,然后把此时的根文件系统设备号ROOT_DEV修改成虚拟盘的设备号. 最后安装根文件系统
	if (NR_HD)
		printk("Partition table%s ok.\n\r",(NR_HD>1)?"s":"");
	rd_load();  // 尝试创建并加载虚拟盘
	mount_root();  // 安装根文件系统
	return (0);
}
// 判断并循环等待硬盘控制器就绪. 读硬盘控制器状态寄存器端口HD_STATUS(0x1f7),循环检测其中的驱动器就绪bit位(位6)是否被置位且控制器忙位
//(位7)是否被复位. 如果返回值 retries=0, 则表示等待控制器空闲的时间已经超时而发生错误,若返回值!=0,则说明在等待(循环)时间期限内控制器
//回到空闲状态. 实际上,仅需要检测状态寄存器忙位(位7)是否为1来判断控制器是否处于忙状态, 驱动器是否就绪(即位6是否为1)与控制器的状态无关.
//因此可以把line165语句改为: while(--retries && (inb_p(HD_STATUS) & 0x80)); 另外由于现在的pc机速度更快,因此可以把等待的循环次数
//再加大一些,例如再增加10倍
static int controller_ready(void)
{
	int retries=10000;

	while (--retries && (inb_p(HD_STATUS)&0xc0)!=0x40); //line165
	return (retries);  // 返回等待循环次数
}
// 检测硬盘执行命令后的状态, win表示温切斯特硬盘的缩写.读取状态寄存器中的命令执行结果状态. 返回0表示正常; 1表示出错. 如果执行命令错,
// 则需要在读错误寄存器HD_ERROR(0x1f1)
static int win_result(void)
{
	int i=inb_p(HD_STATUS);  // 取状态信息

	if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT))
		== (READY_STAT | SEEK_STAT))
		return(0); /* ok */
	if (i&1) i=inb(HD_ERROR);  // 若ERR_STAT置位, 则读取错误寄存器
	return (1);
}
//向硬盘控制器发送命令块. driver:磁盘号(0-1), nsect:读写扇区数; sect:起始扇区; head: 磁头号; cyl:柱面号; cmd:命令码,
//intr_addr()硬盘中断处理程序中将调用的c处理函数指针. 该函数在硬盘控制器就绪后,先设置全局函数指针变量do_hd指向硬盘中断处理程序中将会
//调用的c处理函数, 然后再发送硬盘控制字节和7字节的参数命令块. 硬盘中断处理程序的代码位于kernel/system_call.s中.
static void hd_out(unsigned int drive,unsigned int nsect,unsigned int sect,
		unsigned int head,unsigned int cyl,unsigned int cmd,
		void (*intr_addr)(void))
{
	register int port asm("dx");  // 定义局部寄存器变量并放在指定寄存器dx中
//首先对参数进行有效性检查.如果驱动器号>1(只能是0,1)或者磁头号>15,则程序不支持,停机. 否则就判断并循环等待驱动器就绪.如果等待一段时间
//后仍未就绪则表示硬盘控制器出错,停机.
	if (drive>1 || head>15)
		panic("Trying to write bad sector");
	if (!controller_ready())
		panic("HD controller not ready");
// 接着设置硬盘中断发生时将调用的c函数指针do_hd, 然后在向硬盘控制器发送参数和命令之前, 规定要先向控制器端口命令(0x3f6)发送一指定硬盘
//的控制字节,以建立相应的硬盘控制方式.该控制字节即是硬盘信息结构数组中的ctl字段,然后向控制器端口0x1f1-0x1f7发送7字节的参数命令块
	do_hd = intr_addr;  // do_hd函数会在中断程序中被调用
	outb_p(hd_info[drive].ctl,HD_CMD); // 向控制寄存器输出控制字节
	port=HD_DATA;  // 置dx为数据寄存器端口(0x1f0)
	outb_p(hd_info[drive].wpcom>>2,++port);  // 参数: 写预补偿柱面号(需除以4)
	outb_p(nsect,++port);  // 参数: 读写扇区总数
	outb_p(sect,++port);  // 参数: 起始扇区
	outb_p(cyl,++port);  // 参数: 柱面号低8位
	outb_p(cyl>>8,++port);  // 参数: 柱面号高8位
	outb_p(0xA0|(drive<<4)|head,++port);  // 参数: 驱动器号+磁头
	outb(cmd,++port);  // 命令: 硬盘控制命令
}
// 等待硬盘就绪. 该函数循环等待主状态控制器忙标志位复位,若仅有就绪或寻道结束标志置位,则表示硬盘就绪,成功返回0,若经过一段花时间仍忙则返回1
static int drive_busy(void)
{
	unsigned int i;
//循环读取控制器的主状态寄存器HD_STATUS, 等待就绪标志位置位并且忙位复位. 然后再次读取主控制器状态字节,检测其中忙为、就绪位和寻道结束位
//若仅有就绪或寻道结束标志置位,则表示硬盘就绪,返回0. 否则表示等待超时,于是警告显示信息并返回1
	for (i = 0; i < 10000; i++)
		if (READY_STAT == (inb_p(HD_STATUS) & (BUSY_STAT|READY_STAT)))
			break;
	i = inb(HD_STATUS);  // 再取主控制器状态字节
	i &= BUSY_STAT | READY_STAT | SEEK_STAT;
	if (i == READY_STAT | SEEK_STAT)  // 仅有就绪或寻道结束标志则返回0
		return(0);
	printk("HD controller times out\n\r");  // 否则等待超时,显示信息,并返回1
	return(1);
}
// 诊断复位(重新校正)硬盘控制器
static void reset_controller(void)
{
	int	i;
//首先向控制器寄存器端口0x3f6发送允许复位(4)控制字节, 然后循环空操作等待一段时间让控制器执行复位操作.接着再向该端口发送正常的控制字节
//(不禁止重试、重读),并等待硬盘就绪,如等待硬盘就绪超时,则显示警告信息.然后读取错误寄存器内容,若不等于1(表示无错误)则显示硬盘控制器复位失败信息
	outb(4,HD_CMD);  // 向控制寄存器端口发送复位控制字节
	for(i = 0; i < 100; i++) nop();  // 等待一段时间
	outb(hd_info[0].ctl & 0x0f ,HD_CMD);  // 发送正常控制字节(不禁止重试、重读)
	if (drive_busy())
		printk("HD-controller still busy\n\r");
	if ((i = inb(HD_ERROR)) != 1)
		printk("HD-controller reset failed: %02x\n\r",i);
}
//复位硬盘nr; 首先复位(重新校正)硬盘控制器, 然后发送硬盘控制器命令"建立驱动器参数",其中函数recal_intr()是在硬盘中断处理程序中调用的
//重新校正处理函数.
static void reset_hd(int nr)
{
	reset_controller();
	hd_out(nr,hd_info[nr].sect,hd_info[nr].sect,hd_info[nr].head-1,
		hd_info[nr].cyl,WIN_SPECIFY,&recal_intr);
}
//意外硬盘中断调用函数. 发生意外硬盘中断时,硬盘中断处理程序中调用的默认c处理函数,在被调用函数指针为空时调用该函数.
void unexpected_hd_interrupt(void)
{
	printk("Unexpected HD interrupt\n\r");
}
//读写硬盘失败处理调用函数. 如果读扇区时的出错次数大于或等于7次时, 则结束当前请求项并唤醒等待该请求的进程,而且对应缓冲区更新标志复位,
//表示数据没有更新,如果读一扇区时的出错次数已经大于3次则要求执行复位硬盘控制操作(设置复位标志)
static void bad_rw_intr(void)
{
	if (++CURRENT->errors >= MAX_ERRORS)
		end_request(0);
	if (CURRENT->errors > MAX_ERRORS/2)
		reset = 1;
}
//读操作中断调用函数. 该函数将在硬盘读命令结束时引发的硬盘中断过程中被调用. 在读命令执行后会产生硬盘中断信号,并执行硬盘中断处理程序.此时
//在硬盘中断处理程序中调用的c函数指针do_hd已经指向read_intr(),因此会在一次读扇区操作完成或出错后就会执行该函数.
static void read_intr(void)
{
// 先判断此次读命令操作是否出错, 若命令结束后控制器还处于忙状态,或者命令执行错误,则处理硬盘操作失败问题,接着再次请求硬盘作为作复位处理
//并执行其他请求项. 然后返回. 每次读操作出错都会触发对当前请求项做出错次数累计. 若出错次数不到最大允许出错次数的一半,则会先执行硬盘复位
//操作,然后再执行本次请求项处理.若出错次数已>=最大允许出错次数,则结束本次请求项的处理而去处理队列中下一个请求项
	if (win_result()) {  //控制器忙、读写错或命令自行错,则进行读写硬盘失败处理
		bad_rw_intr();
		do_hd_request();  // 再次请求硬盘做相应(复位)处理
		return;
	}
// 读命令没有错误, 则从数据寄存器端口把1个扇区的数据读到请求项的缓冲区中,且递减请求项所需读取的扇区数值.若递减后!=0,则还有数据未取完,
//于是再次置中断调用c函数指针do_hd=read_intr()并直接返回.等待硬盘在读出另1个扇区数据后发出中断并再次调用本函数,	
	port_read(HD_DATA,CURRENT->buffer,256);  // 256是指内存字,即512字节. 读数据到请求结构缓冲区
	CURRENT->errors = 0;  // 清除出错次数
	CURRENT->buffer += 512;  // 调整缓冲区指针,指向新的空区
	CURRENT->sector++;  // 起始扇区号+1
	if (--CURRENT->nr_sectors) {  // 如果所需读取的扇区数还没有读完,则再置硬盘调用c函数指针为read_intr()
		do_hd = &read_intr; // 再次置do_hd指针指向read_intr,是因为硬盘中断处理程序每次调用do_hd时都会将该函数指针置空
		return;
	}
// 执行到此,说明本次请求项的全部扇区数据已经读完,则调用end_request()去处理请求项结束事宜.最后再次调用do_hd_request()去处理其他硬盘
//请求项,执行其他硬盘请求操作
	end_request(1);  // 数据已更新标志置位1..
	do_hd_request();
}
//写扇区中断调用函数. 该函数将在硬盘写命令结束时引发的硬盘中断过程中被调用. 函数功能与read_intr()类似. 在写命令执行后会产生硬盘中断信号
//并执行硬盘中断处理程序,此时在硬盘中断处理程序中调用的c函数指针do_hd已经指向write_intr(),因此会在一次写扇区操作完成(或出错)后就执行该函数
static void write_intr(void)
{
//先判断此次写命令是否出错,若命令结束后控制器还处于忙状态,或命令执行错误.则处理硬盘操作失败问题,接着再次请求硬盘做复位处理并执行其他请求项	
//返回返回. bad_rw_intr()中每次操作出错都会对当前请求项做 出错次数累计, 若出错次数不到最大允许出错次数的一半则会先执行硬盘复位操作,然后
//再执行本次请求项处理.若出错次数已经>=允许出错次数,则结束本次请求项的处理而去处理队列中下一个请求项,do_hd_request()中会根据当时具体的标志
//状态来判别是否需要先执行复位、重新校正等操作,然后再继续或处理下一个请求项.
	if (win_result()) {  // 如果硬盘控制器返回错误信息
		bad_rw_intr();  // 则首先进行硬盘读写失败错误
		do_hd_request(); // 再次请求硬盘作相应(复位)处理
		return;
	}
//本次写一扇区操作成功,因此将预写扇区数-1,若其不为0,则说明还有扇区要写.于是把当前请求起始扇区号+1,并调整请求项数据缓冲区指针指向下一块
//预写的数据.然后再重置硬盘中断处理程序中调用的c函数指针do_hd. 接着向控制器数据端口写入1512字节数据,然后函数返回去等待控制把这些数据写入
//硬盘后产生的中断.
	if (--CURRENT->nr_sectors) {  // 若还有扇区要写,则
		CURRENT->sector++;  // 当前请求起始扇区号+1
		CURRENT->buffer += 512;  // 调整请求缓冲区指针
		do_hd = &write_intr;  // 置函数指针为 write_intr
		port_write(HD_DATA,CURRENT->buffer,256);  // 向数据端口写256字.
		return;
	}
//本次请求项的全部	扇区数据已经写完毕,则调用end_request()去处理请求项结束事宜.最后再次调用do_hd_request()去处理其他硬盘请求项.
//执行其他硬盘请求操作
	end_request(1);  // 处理请求结束事宜
	do_hd_request();  // 执行其他硬盘请求操作
}
//硬盘重新校正(复位)中断调用函数. 该函数会在硬盘执行重新校正操作而引发的硬盘中断中被调用. 如果硬盘控制器返回错误信息,则函数首先进行硬盘
//读写失败处理,然后请求硬盘作相应(复位)处理,在 bad_rw_intr()函数中, 每次操作出错都会对当前请求项作 出错计数累计, 若出错次数不到最大
//允许出错次数的一半则会先执行硬盘复位操作,然后再执行本次请求项处理.若出错次数已经>=允许出错次数,则结束本次请求项的处理而去处理队列中
//下一个请求项,do_hd_request()中会根据当时具体的状态标志来判断是否需要先执行复位、重新校正等操作,然后在继续或处理下一请求项
static void recal_intr(void)
{
	if (win_result())  // 若返回错误,则调用bad_rw_intr()
		bad_rw_intr();
	do_hd_request();
}
// 执行硬盘读写请求操作; 该函数根据设备当前请求项中的设备号和起始扇区号信息首先计算得到对应硬盘上的柱面号、当前磁道中扇区号、磁头号数据
//然后再根据请求项中的命令(read/write)对硬盘发送相应读、写命令,若控制器复位标志或硬盘重新校正标志已被置位,那么首先去执行复位或重新校正
//操作. 若请求项此时是块设备的第1个(原来设备空闲), 则块设备当前请求项指针会直接指向该请求项,并立刻调用本函数执行读写操作. 否则在一个读写
//操作完成而引发的硬盘中断过程中,若还有请求项需要处理,则也会在硬盘中断过程中调用本函数.
void do_hd_request(void)
{
	int i,r;
	unsigned int block,dev;
	unsigned int sec,head,cyl;
	unsigned int nsect;
//检测请求项的合法性.若请求队列中已没有请求项则退出. 然后取设备号中的子设备号以及设备当前请求项中的开始扇区号.子设备号即对应硬盘上各分区
//如果子设备号不存在或起始扇区>该分区扇区数-2,则结束该请求项,并跳转到repeat处,因为一次要求读写一块数据(2个扇区,1024字节),所以请求的扇区
//号不能 > 分区中最后倒数第二个扇区号, 然后通过加上子设备号对应分区的开始扇区号,就把需要读写的块对应到整个硬盘的绝对扇区号block上,
//而子设备号被5整除即可得到对应的硬盘号.
	INIT_REQUEST;
	dev = MINOR(CURRENT->dev);
	block = CURRENT->sector;  // 请求的开始扇区
	if (dev >= 5*NR_HD || block+2 > hd[dev].nr_sects) {
		end_request(0);
		goto repeat;  // 该标号在blk.h
	}
	block += hd[dev].start_sect;
	dev /= 5;  // 此时dev代表硬盘号(硬盘0还是硬盘1)
//根据求得的绝对扇区号block和硬盘号dev,就可以计算处对应硬盘中的磁道中扇区号(sec),所在柱面号(cyl)和磁头号(head).
//汇编代码即用来根据硬盘结构信息中的每磁道扇区数和硬盘磁头数来计算这些数据,方法是: 初始时eax是扇区号block, edx=0, divl 把edx:eax
//组成的扇区号除以每磁道扇区数(hd_info[dev].sect), 所得整数商值在eax中,余数在edx中,其中eax中是到指定位置的对应总磁道数(所有磁头面)
//edx中是当前磁道上的扇区号.
	__asm__("divl %4":"=a" (block),"=d" (sec):"0" (block),"1" (0),
		"r" (hd_info[dev].sect));
///eax是计算出的对应总磁道数,edx=0,divl把edx:eax的对应总磁道数除以硬盘总磁头数(hd_info[dev].head),在eax中得到的整除值是柱面号
//cyl,edx中得到的余数是对应得当前磁头号head.
	__asm__("divl %4":"=a" (cyl),"=d" (head):"0" (block),"1" (0),
		"r" (hd_info[dev].head));
	sec++; // 对计算所得当前磁道扇区号进行调整
	nsect = CURRENT->nr_sectors;  // 预读写的扇区数
//此时得到了预读写的硬盘开始扇区block所对应的硬盘上柱面号(cyl)、在当前磁道上的扇区号(sec),磁头号(head)以及预读写的总扇区数(nsect),
//接着可以根据这些信息向硬盘控制器发送IO操作信息了. 但在发送之前都还需要看看是否有复位控制器状态和重新校正硬盘的标志. 通常在复位操作之后
//都需要重新校正硬盘磁头位置,若这些标志以被置位,则说明前面的硬盘操作可能出了一些问题,或者现在是系统第一次硬盘读写操作情况,于是就需要重新
//复位硬盘或控制器并重新校正硬盘. 如果此时复位标志 reset 是置位的,则需要执行复位操作,复位硬盘和控制器,并置硬盘需要重新校正标志,返回.
//reset_hd()将首先向硬盘控制器发送复位(重新校正)命令,然后发送硬盘控制器命令"建立驱动器参数".
	if (reset) {
		reset = 0;
		recalibrate = 1;  // 置需重新校正标志
		reset_hd(CURRENT_DEV);
		return;
	}
// 如果此时重新校正标志是置位的,则首先复位该标志,然后向硬盘控制器发送重新校正命令,该命令会执行寻道操作,让处于任何地方的磁头移动到0柱面	
	if (recalibrate) {
		recalibrate = 0;
		hd_out(dev,hd_info[CURRENT_DEV].sect,0,0,0,
			WIN_RESTORE,&recal_intr);
		return;
	}	
//开始向硬盘控制发送真正的数据读写操作命令. 如果当前请求是写扇区操作,则发送写命令,循环读取状态寄存器信息并判断请求服务标志DRA_STAT是否
//置位. DRQ_STAT是硬盘状态寄存器的请求服务位,表示驱动器已经准备号在主机和数据端口之间传输一个子或一个字节的数据.
	if (CURRENT->cmd == WRITE) {
		hd_out(dev,nsect,sec,head,cyl,WIN_WRITE,&write_intr);
		for(i=0 ; i<3000 && !(r=inb_p(HD_STATUS)&DRQ_STAT) ; i++)
			/* nothing */ ;
// 如果请求服务DRQ置位则退出循环,若等到循环结束也没有置位,则表示发送的要求写硬盘命令失败.于是跳转去处理出现的问题或继续执行下一个硬盘
//请求,否则就可以向硬盘控制器数据寄存器端口 HD_DATA写入1个扇区的数据
		if (!r) {
			bad_rw_intr();
			goto repeat;
		}
		port_write(HD_DATA,CURRENT->buffer,256);
	} else if (CURRENT->cmd == READ) {  // 如果当前请求是读硬盘数据,则向硬盘控制器发送读扇区命令,若命令无效则停机
		hd_out(dev,nsect,sec,head,cyl,WIN_READ,&read_intr);
	} else
		panic("unknown hd-command");
}
//硬盘系统初始化; 设置硬盘中断描述符,并允许硬盘控制器发送中断请求信号. 该函数设置硬盘设备的请求项处理函数指针为do_hd_request, 然后设置
//中断门描述符, hd_interrupt 是其中断处理过程. 硬盘中断号为 int 0x2e(46), 对应8259A芯片的中断请求信号IRQ13. 接着复位接连的主8259A
// int2的屏蔽位, 允许从片发出中断请求信号. 再复位硬盘的中断请求屏蔽位(在从片上), 允许硬盘控制器发送中断请求信号, 中断描述符表IDT内中断
//门描述符设置宏 set_intr_gate()在 include/asm/system.h 中
void hd_init(void)
{
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;  // do_hd_request()
	set_intr_gate(0x2E,&hd_interrupt);
	outb_p(inb_p(0x21)&0xfb,0x21);  // 复位连接的主 8259A int2 的屏蔽位
	outb(inb_p(0xA1)&0xbf,0xA1);  // 复位硬盘中断请求屏蔽位(在从片上)
}
