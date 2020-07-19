//和开启子进程相关
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>   //信号相关头文件 
#include <errno.h>    //errno
#include <unistd.h>

#include "mkd_func.h"
#include "mkd_macro.h"
#include "mkd_c_conf.h"

//函数声明
static void mkd_start_worker_processes(int threadnums);
static int mkd_spawn_process(int threadnums,const char *pprocname);
static void mkd_worker_process_cycle(int inum,const char *pprocname);
static void mkd_worker_process_init(int inum);

//变量声明
static u_char  master_process[] = "master process";

//创建worker子进程
void mkd_master_process_cycle()
{    
    sigset_t set;        //信号集

    sigemptyset(&set);   //清空信号集

    //下列这些信号在执行本函数期间不希望收到，防止信号的干扰；
    sigaddset(&set, SIGCHLD);     //子进程状态改变
    sigaddset(&set, SIGALRM);     //定时器超时
    sigaddset(&set, SIGIO);       //异步I/O
    sigaddset(&set, SIGINT);      //终端中断符
    sigaddset(&set, SIGHUP);      //连接断开
    sigaddset(&set, SIGUSR1);     //用户定义信号
    sigaddset(&set, SIGUSR2);     //用户定义信号
    sigaddset(&set, SIGWINCH);    //终端窗口大小改变
    sigaddset(&set, SIGTERM);     //终止
    sigaddset(&set, SIGQUIT);     //终端退出符

    //设置，此时无法接受的信号，阻塞期间，发过来的上述信号，多个会被合并为一个，暂存着，等放开信号屏蔽后才能收到这些信号
    if (sigprocmask(SIG_BLOCK, &set, NULL) == -1) 
    {        
        mkd_log_error_core(MKD_LOG_ALERT,errno,"mkd_master_process_cycle()中sigprocmask()失败!");
    }

    //设置主进程标题
    size_t size;
    int    i;
    size = sizeof(master_process);  
    size += g_argvneedmem;         
    if(size < 1000)
    {
        char title[1000] = {0};
        strcpy(title,(const char *)master_process); //"master process"
        strcat(title," ");   //"master process "
        for (i = 0; i < g_os_argc; i++)         //"master process ./mkdyd"
        {
            strcat(title,g_os_argv[i]);
        }
        mkd_setproctitle(title); //设置标题
        mkd_log_error_core(MKD_LOG_NOTICE,0,"%s %P 【master进程】启动并开始运行......!",title,mkd_pid); //设置标题时记录下来进程名，进程id等信息到日志
    }    
        
    //从配置文件中读取要创建的worker进程数量
    CConfig *p_config = CConfig::GetInstance(); //单例类
    int workprocess = p_config->GetIntDefault("WorkerProcesses",1); //从配置文件中得到要创建的worker进程数量
    mkd_start_worker_processes(workprocess);  //创建worker子进程

    //创建子进程后，父进程的执行流程会返回到这里，子进程不会走进来    
    sigemptyset(&set); //信号屏蔽字为空，表示不屏蔽任何信号
    
    for ( ;; ) 
    {
        sigsuspend(&set); //阻塞在这里，等待一个信号
        sleep(1); //休息1秒        

    }
    return;
}

//描述：根据给定的参数创建指定数量的子进程
static void mkd_start_worker_processes(int threadnums)
{
    int i;
    for (i = 0; i < threadnums; i++)  //master进程走这个循环，来创建若干个子进程
    {
        mkd_spawn_process(i,"worker process");
    } 
    return;
}

//产生一个子进程
static int mkd_spawn_process(int inum,const char *pprocname)
{
    pid_t  pid;

    pid = fork(); //fork()系统调用产生子进程
    switch (pid)  //pid判断父子进程，分支处理
    {  
    case -1: //产生子进程失败
        mkd_log_error_core(MKD_LOG_ALERT,errno,"mkd_spawn_process()fork()产生子进程num=%d,procname=\"%s\"失败!",inum,pprocname);
        return -1;

    case 0:  //子进程分支
        mkd_parent = mkd_pid;              
        mkd_pid = getpid();                
        mkd_worker_process_cycle(inum,pprocname);    //所有worker子进程，在这个函数里不断循环
        break;

    default: //父进程分支，直接break          
        break;
    }
    return pid;
}

//worker子进程的功能函数
static void mkd_worker_process_cycle(int inum,const char *pprocname) 
{
    //设置一下变量
    mkd_process = MKD_PROCESS_WORKER;  //设置进程的类型，是worker进程

    //为子进程设置进程名
    mkd_worker_process_init(inum);
    mkd_setproctitle(pprocname); //设置标题   
    mkd_log_error_core(MKD_LOG_NOTICE,0,"%s %P 【worker进程】启动并开始运行......!",pprocname,mkd_pid); 
    for(;;)
    {
        mkd_process_events_and_timers(); //处理网络事件和定时器事件
    } 

    g_threadpool.StopAll();      //停止线程池；
    g_socket.Shutdown_subproc(); //socket需要释放的东西释放
    return;
}

//子进程初始化
static void mkd_worker_process_init(int inum)
{
    sigset_t  set;      //信号集

    sigemptyset(&set);  //清空信号集
    if (sigprocmask(SIG_SETMASK, &set, NULL) == -1)  //原来是屏蔽那10个信号【防止fork()期间收到信号导致混乱】，现在不再屏蔽任何信号【接收任何信号】
    {
        mkd_log_error_core(MKD_LOG_ALERT,errno,"mkd_worker_process_init()中sigprocmask()失败!");
    }

    //线程池代码
    CConfig *p_config = CConfig::GetInstance();
    int tmpthreadnums = p_config->GetIntDefault("ProcMsgRecvWorkThreadCount",5); //处理接收到的消息的线程池中线程数量
    if(g_threadpool.Create(tmpthreadnums) == false)  //创建线程池中线程
    {
        exit(-2);
    }
    sleep(1); //再休息1秒；

    if(g_socket.Initialize_subproc() == false) //初始化子进程需要具备的一些多线程能力相关的信息
    {
        exit(-2);
    }
    
    g_socket.mkd_epoll_init();           //初始化epoll相关内容，同时往监听socket上增加监听事件
    
    return;
}
