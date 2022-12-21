/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */
//bitmap.c含有处理i节点和磁盘块位图的代码
/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>
//将指定地址(addr)处的一块1024字节内存清空; eax=0; ecx=以长字为单位的数据块长度(block_size/4);
//edi 指定起始地址addr
#define clear_block(addr) \
__asm__("cld\n\t" \
	"rep\n\t" \  //重复执行存储数据0
	"stosl" \
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")
//把指定地址开始的第nr个位偏移处的bit位置位(nr可大于32), 返回原bit位值. %0 eax 返回值;
//%2 nr 位偏移值; %3 addr addr的内容; 定义了一个局部寄存器变量res, 该变量将被保存在指定的
//eax寄存器中; 整个宏是一个语句表达式(即圆括号括住的组号语句), 其值是组号语句中最后一条表达式
//res的值. btsl 指令用于测试并设置bit位(bit test and set), 把基地址(%3)和比特位偏移值(%2)
//所指定的比特位值先保存到进位标志CF中,然后色湖之该比特位=1,指令setb用于根据CF设置操作数%al
//如果CF=1, 则al=1, 否则al=0
#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})
//复位指定地址开始的第nr位偏移处的比特位,返回原比特位值的反码. %0 eax 返回值; %1 eax 0
//%2 nr 位偏移值; %e addr addr的内容. btrl 指令用于测试并复位比特位(bit test and reset)
//setnb 用于根据CF设置操作数al, CF=1,则al=0, 否则al=1
#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})
//从addr开始寻找第1个0值比特位; %0 ecx 返回值; %1 ecx 0; %2 esi addr; 在addr指定地址开始的
//位图中寻找第1个是0的比特位,将其距离addr的比特位偏移值返回. addr是缓冲块数据区的地址,扫描
//寻找的范围是 1024 字节(8192比特位)
#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \
	"1:\tlodsl\n\t" \  //取[esi]-> eax
	"notl %%eax\n\t" \ //eax中每位取反
	"bsfl %%eax,%%edx\n\t" \ //从位0扫描eax中是1的第1个位,其偏移值 -> edx
	"je 2f\n\t" \  // 如果eax中全是0, 则向前跳转到标号2处
	"addl %%edx,%%ecx\n\t" \  //偏移值加入ecx(ecx是位图首个0值位的偏移值)
	"jmp 3f\n" \  //向前跳转到标号3处
	"2:\taddl $32,%%ecx\n\t" \  //未找到0值位,则将ecx加1个长字的位偏移量32
	"cmpl $8192,%%ecx\n\t" \ //已经扫描了8192比特位(1024字节)了吗?
	"jl 1b\n" \  // 若还没有扫描完1块数据,则向后跳转到标号1处
	"3:" \ //结束,此时ecx中是位偏移量
	:"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})
// 释放设备dev上数据区中的逻辑块block; 复位指定逻辑块block对应的逻辑块位图bit位.
//dev是设备号, block是逻辑块号(盘块号)
void free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;
//先取设备dev上文件系统的超级块信息,根据其中数据区开始逻辑块号和文件系统中逻辑块总数信息
//判断参数block的有效性. 如果指定设备超级块不存在,则出错停机.若逻辑块号<盘上数据区第1个
//逻辑块的块号或>设备上总逻辑块数, 也出错停机
	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
//从hash表中寻找该块数据, 找到了则判断其有效性并清已修改和更新标志,释放该数据块. 该段代码
//的主要用途是如果该逻辑块目前存在于高速缓冲区中, 就释放对应的缓冲块; 下面代码中line56到
//line66有问题, 会造成数据块不能释放. 因为当 b_count>1时,这段代码会仅打印一段信息而没有
//执行释放操作, 应改动如下:
/*if(bh) {
	if(bh->b_count > 1) { //如果引用次数>1,则调用brelease(),
	  brelse(bh); //b_count-- 后立即退出, 该块还有人用
	  return 0;
	}
	bh->b_dirt = 0; //复位已修改和已更新标志;
	bh->b_update = 0;
	if(bh->b_count) brelse(bh);  // 此时b_count=1, 则调用brelse()释放之.
}	
*/	
	bh = get_hash_table(dev,block); //line56
	if (bh) {
		if (bh->b_count != 1) {
			printk("trying to free block (%04x:%d), count=%d\n",
				dev,block,bh->b_count);
			return;
		}
		bh->b_dirt=0;  // 复位脏(已修改)标志位
		bh->b_uptodate=0;  // 复位更新标志
		brelse(bh);
	} //line66
//接着复位block在逻辑块位图中的比特位(置0), 先计算block在数据区开始算起的数据逻辑块(从1开始)
//然后对逻辑块(区块)位图进行操作,复位对应的比特位. 如果对应比特位原来就是0,则出错停机. 由于
//1个缓冲块有1024字节,即8192比特位, 因此block/8192即可计算出指定块block在逻辑位图中的哪个
//块上. block&8191 可以得到block在逻辑块位图当前块中的比特偏移位置
	block -= sb->s_firstdatazone - 1 ;
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		panic("free_block: bit already cleared");
	}
	sb->s_zmap[block/8192]->b_dirt = 1; //最后置相应逻辑块位图所在缓冲区已修改标志
}
//向设备申请一个逻辑块(盘块,区块). 函数首先取得设备的超级块,并在超级块中的逻辑块位图中寻找第
//一个0值比特位(代表一个空闲逻辑块). 然后置位对应逻辑块在逻辑块位图中的比特位. 接着为该逻辑块
//在缓冲区中取得一块对应缓冲块. 最后将该缓冲块清零,并设置其已更新标志和已修改标志,并返回逻辑
//块号. 函数执行成功则返回逻辑块号(盘块号),否则返回0
int new_block(int dev)
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;
//获取设备dev的超级块,如果指定设备的超级块不存在,则出错停机. 然后扫描文件系统的8块逻辑块位图
//寻找首个0值比特位, 以寻找空闲逻辑块,获取放置该逻辑块的块号, 如果全部扫描完8块逻辑块位图的
//所有比特位(i>=8或j>=8192)还没有找到0值比特位或者位图所在的缓冲块指针无效(bh=NULL)则返回0
//退出(没有空闲逻辑块)
	if (!(sb = get_super(dev)))
		panic("trying to get new block from nonexistant device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_zmap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (i>=8 || !bh || j>=8192)
		return 0;
//设置找到的新逻辑块j对应逻辑块位图中的bit位,若对应比特位已置位,则出错停机. 否则置存放位图的
//对应缓冲块已修改标志. 因为逻辑块位图仅表示盘上数据区中逻辑块的占用情况, 即逻辑块位图中bit位
//偏移值表示从数据区开始处算起的块号,因此这里需要加上数据区第1个逻辑块的块号, 把j转换成逻辑
//块号. 此时如果新逻辑块大于该设备上的总逻辑块数, 则说明指定逻辑块在对应设备上不存在,
//申请失败, 返回0退出
	if (set_bit(j,bh->b_data))
		panic("new_block: bit already set");
	bh->b_dirt = 1;
	j += i*8192 + sb->s_firstdatazone-1;
	if (j >= sb->s_nzones)
		return 0;
//在高速缓冲区中为该设备上指定的逻辑块号取得一个缓冲块,并返回缓冲块头指针. 因为刚取得的逻辑块
//其引用次数一定为1(getblk()中会设置), 因此若不为1则停机. 最后将新逻辑块清0,并设置其已更新
//标志和已修改标志. 然后释放对应缓冲块, 返回逻辑块号
	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");
	clear_block(bh->b_data);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	brelse(bh);
	return j;
}
//释放指定的i节点; 首先判断参数给出的i节点号的有效性和可释放性.若i节点仍然在使用中则不能
//被释放. 然后利用超级块信息对i节点位图进行操作, 复位i节点号对应的i节点位图中bit位,并清空i节点结构
void free_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
//判断参数给出的需要释放的i节点有效性或合法性. 如果i节点指针=NULL, 则退出. 如果i节点上的
//设备号字段=0,则说明该节点没有使用, 于是用0清空对应i节点所占内存区并返回. 这里用0填写
//inode指针指定处、长度时sizeof(*inode)的内存块
	if (!inode)
		return;
	if (!inode->i_dev) {
		memset(inode,0,sizeof(*inode));
		return;
	}
//如果此时i节点还有其他程序引用, 则不能释放, 说明内核有问题,停机. 如果文件连接数!=0, 则表示
//还有其他文件目录项在使用该节点,因此也不应释放,而应该放回
	if (inode->i_count>1) {
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	if (inode->i_nlinks)
		panic("trying to free inode with links");
//判断完i节点的合理性后, 开始利用超级块信息对其中的i节点位图进行操作. 先取i节点所在设备的
//超级块,测试块设备是否存在, 然后判断i节点号的范围是否正确,如果i节点号=0或 >该设备上i节点
//总数,则出错(0号节点保留没有使用). 如果该i节点对应的节点位图不存在,则出错. 因为一个缓冲块
//的i节点位图有8192比特位, 因此i_num >> 13(即i_num/8192)可以得到当前i节点号所在的s_imap[]
//项,即所在盘块
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to free inode on nonexistent device");
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
		panic("trying to free inode 0 or nonexistant inode");
	if (!(bh=sb->s_imap[inode->i_num>>13]))
		panic("nonexistent imap in superblock");
//复位i节点对应的节点位图中的比特位. 如果该比特位已经=0, 则显示出错信息,最后置i节点位图所在缓冲区
//已修改标志,并清空该i节点结构所占内存区.
	if (clear_bit(inode->i_num&8191,bh->b_data))
		printk("free_inode: bit already cleared.\n\r");
	bh->b_dirt = 1;
	memset(inode,0,sizeof(*inode));
}
//位设备建立一个新i节点,初始化并返回该新i节点的指针; 在内存i节点表中获取一个空闲i节点表项
//从i节点位图中找一个空闲i节点
struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;
//先从内存i节点表(inode_table)中获取一个空闲i节点项,并读取指定设备的超级块结构,然后扫描
//超级块中8块i节点位图,寻找首个0比特位,寻找空闲节点,获取放置该i节点的节点号. 如果全部扫描
//还没找到,或位图所在的缓冲块无效(bh=NULL),则放回先前申请的i节点表中的i节点,并返回空指针
//退出(没有空闲i节点)
	if (!(inode=get_empty_inode()))
		return NULL;
	if (!(sb = get_super(dev)))
		panic("new_inode with unknown device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
//已经找到了还未使用的i节点号j, 于是置i节点j对应的i节点位图相应比特位(如果已置位则出错),
//然后置i节点位图所在缓冲块已修改标志, 最后初始化该i节点结构(i_ctime时i节点内容改变时间)
	if (set_bit(j,bh->b_data))
		panic("new_inode: bit already set");
	bh->b_dirt = 1;
	inode->i_count=1;  //引用计数
	inode->i_nlinks=1;  //文件目录项链接数
	inode->i_dev=dev; //i节点所在的设备号
	inode->i_uid=current->euid; //i节点所属用户id
	inode->i_gid=current->egid; //组id
	inode->i_dirt=1;  // 已修改标志置位
	inode->i_num = j + i*8192; //对应设备中的i节点号
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;  //设置时间
	return inode; //返回该i节点指针
}
