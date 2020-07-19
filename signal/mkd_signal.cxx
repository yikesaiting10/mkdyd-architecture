//和信号相关
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>    //信号相关头文件 
#include <errno.h>     //errno
#include <sys/wait.h>  //waitpid

#include "mkd_global.h"
#include "mkd_macro.h"
#include "mkd_func.h" 

//一个信号有关的结构
typedef struct 
{
    int           signo;       //信号对应的数字编号 
    const  char   *signame;    //信号对应的中文名字 ，比如SIGHUP 

    //信号处理函数
    void  (*handler)(int signo, siginfo_t *siginfo, void *ucontext); //函数指针
} mkd_signal_t;

//声明一个信号处理函数
static void mkd_signal_handler(int signo, siginfo_t *siginfo, void *ucontext); 
static void mkd_process_get_status(void);                                      //获取子进程的结束状态，防止单独kill子进程时子进程变成僵尸进程

//数组 ，定义本系统处理的各种信号
mkd_signal_t  signals[] = {
    // signo      signame             handler
    { SIGHUP,    "SIGHUP",           mkd_signal_handler },        //终端断开信号，对于守护进程常用于reload重载配置文件通知--标识1
    { SIGINT,    "SIGINT",           mkd_signal_handler },        //标识2   
	{ SIGTERM,   "SIGTERM",          mkd_signal_handler },        //标识15
    { SIGCHLD,   "SIGCHLD",          mkd_signal_handler },        //子进程退出时，父进程会收到这个信号--标识17
    { SIGQUIT,   "SIGQUIT",          mkd_signal_handler },        //标识3
    { SIGIO,     "SIGIO",            mkd_signal_handler },        //指示一个异步I/O事件【通用异步I/O信号】
    { SIGSYS,    "SIGSYS, SIG_IGN",  NULL               },        //忽略这个信号，SIGSYS表示收到了一个无效系统调用，如果不忽略，进程会被操作系统杀死--标识31
                                                                  //把handler设置为NULL，代表要求忽略这个信号
    { 0,         NULL,               NULL               }         //信号对应的数字至少是1，用0作为一个特殊标记
};

//初始化信号的函数，用于注册信号处理程序
int mkd_init_signals()
{
    mkd_signal_t      *sig;  //指向自定义结构数组的指针 
    struct sigaction   sa;   //sigaction：系统定义的跟信号有关的一个结构

    for (sig = signals; sig->signo != 0; sig++)  
    {        
        memset(&sa,0,sizeof(struct sigaction));

        if (sig->handler) 
        {
            sa.sa_sigaction = sig->handler;  //sa_sigaction：指定信号处理程序(函数)
            sa.sa_flags = SA_SIGINFO;        //sa_flags：int型，指定信号的一些选项
        }
        else
        {
            sa.sa_handler = SIG_IGN; //sa_handler:这个标记SIG_IGN给到sa_handler成员，表示忽略信号的处理程序
        } 

        sigemptyset(&sa.sa_mask);   
                                    
        //设置信号处理动作
        if (sigaction(sig->signo, &sa, NULL) == -1) 
        {   
            mkd_log_error_core(MKD_LOG_EMERG,errno,"sigaction(%s) failed",sig->signame); //显示到日志文件中去的 
            return -1; 
        }	
        else
        {            
            //do nothing
        }
    } 
    return 0;     
}

//信号处理函数
static void mkd_signal_handler(int signo, siginfo_t *siginfo, void *ucontext)
{       
    mkd_signal_t    *sig;    //自定义结构
    char            *action; //一个字符串，用于记录一个动作字符串以往日志文件中写
    
    for (sig = signals; sig->signo != 0; sig++) //遍历信号数组    
    {         
        //找到对应信号
        if (sig->signo == signo) 
        { 
            break;
        }
    } 

    action = (char *)"";  //目前还没有什么动作

    if(mkd_process == MKD_PROCESS_MASTER)      //master进程，管理进程
    {
        switch (signo)
        {
        case SIGCHLD:  //一般子进程退出会收到该信号
            mkd_reap = 1;  //标记子进程状态变化
            break;
        default:
            break;
        } 
    }
    else if(mkd_process == MKD_PROCESS_WORKER) //worker进程
    {
        //....
    }
    else
    {
        //非master非worker进程
        //do nothing
    }

    //记录一些日志信息
    if(siginfo && siginfo->si_pid)  //si_pid = sending process ID【发送该信号的进程id】
    {
        mkd_log_error_core(MKD_LOG_NOTICE,0,"signal %d (%s) received from %P%s", signo, sig->signame, siginfo->si_pid, action); 
    }
    else
    {
        mkd_log_error_core(MKD_LOG_NOTICE,0,"signal %d (%s) received %s",signo, sig->signame, action);
    }

    //子进程状态有变化，通常是意外退出
    if (signo == SIGCHLD) 
    {
        mkd_process_get_status(); //获取子进程的结束状态
    } 

    return;
}

//获取子进程的结束状态，防止单独kill子进程时子进程变成僵尸进程
static void mkd_process_get_status(void)
{
    pid_t            pid;
    int              status;
    int              err;
    int              one=0; 

    //当杀死一个子进程时，父进程会收到这个SIGCHLD信号
    for ( ;; ) 
    {
        pid = waitpid(-1, &status, WNOHANG); //第一个参数为-1，表示等待任何子进程，
                                              //第二个参数：保存子进程的状态信息
                                               //第三个参数：提供额外选项，WNOHANG表示不要阻塞        

        if(pid == 0) //子进程没结束，会立即返回这个数字
        {
            return;
        } 
        if(pid == -1)//表示这个waitpid调用有错误
        {
            err = errno;
            if(err == EINTR)           //调用被某个信号中断
            {
                continue;
            }

            if(err == ECHILD  && one)  //没有子进程
            {
                return;
            }

            if (err == ECHILD)         //没有子进程
            {
                mkd_log_error_core(MKD_LOG_INFO,err,"waitpid() failed!");
                return;
            }
            mkd_log_error_core(MKD_LOG_ALERT,err,"waitpid() failed!");
            return;
        }  
        //走到这里，表示成功返回进程id
        one = 1;  //标记waitpid()返回了正常的返回值
        if(WTERMSIG(status))  //获取使子进程终止的信号编号
        {
            mkd_log_error_core(MKD_LOG_ALERT,0,"pid = %P exited on signal %d!",pid,WTERMSIG(status)); //获取使子进程终止的信号编号
        }
        else
        {
            mkd_log_error_core(MKD_LOG_NOTICE,0,"pid = %P exited with code %d!",pid,WEXITSTATUS(status)); //WEXITSTATUS()获取子进程传递给exit或者_exit参数的低八位
        }
    } 
    return;
}
