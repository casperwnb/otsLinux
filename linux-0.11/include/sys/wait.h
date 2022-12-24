#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>

#define _LOW(v)		( (v) & 0377) //取低字节(8进制表示)
#define _HIGH(v)	( ((v) >> 8) & 0377) //取高字节

/* options for waitpid, WUNTRACED not supported */
//waitpid的选项,linux0.11以及支持了 WUNTRACED
#define WNOHANG		1 //如果没有状态也不要挂起,并立刻返回
#define WUNTRACED	2 //报告停止执行的子进程状态
//以下宏定义用于判断waitpid()返回的状态字含义
#define WIFEXITED(s)	(!((s)&0xFF) //如果子进程正常退出, 则为真
#define WIFSTOPPED(s)	(((s)&0xFF)==0x7F) // 如果子进程正停止着, 则为true
#define WEXITSTATUS(s)	(((s)>>8)&0xFF) //返回退出状态
#define WTERMSIG(s)	((s)&0x7F) // 返回导致进程终止的信号值(信号量)
#define WSTOPSIG(s)	(((s)>>8)&0xFF) //返回导致进程停止的信号值
#define WIFSIGNALED(s)	(((unsigned int)(s)-1 & 0xFFFF) < 0xFF) //如果由于未捕捉到信号,而导致子进程退出则为真
//wait(), waitpid()允许进程获取与其子进程之一的状态信息. 各种选项允许获取已经终止或停止的子进程状态信息.如果存在两个或两个以上子进程的
//状态信息,则报告的顺序是不指定的. wait()将挂起当前进程,直到器子进程之一退出(终止),或者收到要求终止该进程的信号.或者是需要调用一个信号
//句柄(信号处理程序); waitpid()挂起当前进程,直到pid指定的子进程退出(终止)或者收到要求终止该进程的信号,或者是需要调用一个信号句柄(信号
//处理程序). 如果pid=-1, options=0,则waitpid()的作用与wait()一样, 否则其行为将随pid和options参数的不同而不同.
pid_t wait(int *stat_loc);
pid_t waitpid(pid_t pid, int *stat_loc, int options);

#endif
