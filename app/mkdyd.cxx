
//整个程序入口
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h> 
#include <errno.h>
#include <arpa/inet.h>

#include "mkd_macro.h"         //各种宏定义
#include "mkd_func.h"          //各种函数声明
#include "mkd_c_conf.h"        //和配置文件处理相关的类,名字带c_表示和类有关
#include "mkd_c_socket.h"      //和socket通讯相关
#include "mkd_c_memory.h"      //和内存分配释放等相关
#include "mkd_c_threadpool.h"  //和多线程有关
#include "mkd_c_crc32.h"       //和crc32校验算法有关 
#include "mkd_c_slogic.h"      //和socket通讯相关

//本文件用的函数声明
static void freeresource();

//和设置标题有关的全局量
size_t  g_argvneedmem=0;        //保存下这些argv参数所需要的内存大小
size_t  g_envneedmem=0;         //环境变量所占内存大小
int     g_os_argc;              //参数个数 
char    **g_os_argv;            //原始命令行参数数组,在main中会被赋值
char    *gp_envmem=NULL;        //指向自己分配的env环境变量的内存，在mkd_init_setproctitle()函数中会被分配内存
int     g_daemonized=0;         //守护进程标记，标记是否启用了守护进程模式，0：未启用，1：启用了

//socket/线程池相关        
CLogicSocket   g_socket;        //socket全局对象  
CThreadPool    g_threadpool;    //线程池全局对象

//和进程本身有关的全局量
pid_t   mkd_pid;                //当前进程的pid
pid_t   mkd_parent;             //父进程的pid
int     mkd_process;            //进程类型，比如master,worker进程等
int     g_stopEvent;            //标志程序退出,0不退出1，退出

sig_atomic_t  mkd_reap;         //标记子进程状态变化[一般是子进程发来SIGCHLD信号表示退出],sig_atomic_t:系统定义的类型：访问或改变这些变量需要在计算机的一条指令内完成
                                   //一般等价于int【通常情况下，int类型的变量通常是原子访问的，也可以认为 sig_atomic_t就是int类型的数据】                                   

//程序主入口函数----------------------------------
int main(int argc, char *const *argv)
{     
    int exitcode = 0;           //退出代码，先给0表示正常退出
    int i;                      //临时用
    //初始化的变量
    g_stopEvent = 0;            //标记程序是否退出，0不退出     

    mkd_pid    = getpid();      //取得进程pid
    mkd_parent = getppid();     //取得父进程的id 
    //统计argv所占的内存
    g_argvneedmem = 0;
    for(i = 0; i < argc; i++)  
    {
        g_argvneedmem += strlen(argv[i]) + 1;
    } 
    //统计环境变量所占的内存
    for(i = 0; environ[i]; i++) 
    {
        g_envneedmem += strlen(environ[i]) + 1; 
    } 

    g_os_argc = argc;           //保存参数个数
    g_os_argv = (char **) argv; //保存参数指针

    //初始化全局量
    mkd_log.fd = -1;                  //-1：表示日志文件尚未打开
    mkd_process = MKD_PROCESS_MASTER; //先标记本进程是master进程
    mkd_reap = 0;                     //标记子进程没有发生变化
   
    //初始化失败就要直接退出
    //读配置文件
    CConfig *p_config = CConfig::GetInstance(); //单例类
    if(p_config->Load("mkdyd.conf") == false) //把配置文件内容载入到内存            
    {   
        mkd_log_init();    //初始化日志
        mkd_log_stderr(0,"配置文件[%s]载入失败，退出!","mkdyd.conf");
        exitcode = 2; //标记找不到文件
        goto lblexit;
    }
    //内存单例类初始化
    CMemory::GetInstance();	
    //crc32校验算法单例类初始化
    CCRC32::GetInstance();
        
    //日志初始化
    mkd_log_init();                  
        
    //初始化部分函数       
    if(mkd_init_signals() != 0) //信号初始化
    {
        exitcode = 1;
        goto lblexit;
    }        
    if(g_socket.Initialize() == false)//初始化socket
    {
        exitcode = 1;
        goto lblexit;
    }

    mkd_init_setproctitle();    //把环境变量搬家

    //创建守护进程
    if(p_config->GetIntDefault("Daemon",0) == 1) //读配置文件，拿到配置文件中是否按守护进程方式启动的选项
    {
        //按守护进程方式运行
        int cdaemonresult = mkd_daemon();
        if(cdaemonresult == -1) //fork()失败
        {
            exitcode = 1;    //标记失败
            goto lblexit;
        }
        if(cdaemonresult == 1)
        {
            //这是原始的父进程
            freeresource();   //只有进程退出了才goto到 lblexit，用于提醒用户进程退出了
            exitcode = 0;
            return exitcode;  //整个进程直接在这里退出
        }
        //走到这里，成功创建了守护进程并且这里已经是fork()出来的进程，现在这个进程做master进程
        g_daemonized = 1;    //守护进程标记，标记是否启用了守护进程模式，0：未启用，1：启用了
    }

    //开始正式的主工作流程    
    mkd_master_process_cycle(); //不管父进程还是子进程，正常工作期间都在这个函数里循环；
        
lblexit:
    //释放资源
    mkd_log_stderr(0,"程序退出，再见了!");
    freeresource();  
    return exitcode;
}

//专门在程序执行末尾释放资源的函数
void freeresource()
{
    //对于因为设置可执行程序标题导致的环境变量分配的内存应该释放
    if(gp_envmem)
    {
        delete []gp_envmem;
        gp_envmem = NULL;
    }

    //关闭日志文件
    if(mkd_log.fd != STDERR_FILENO && mkd_log.fd != -1)  
    {        
        close(mkd_log.fd); 
        mkd_log.fd = -1;      
    }
}
