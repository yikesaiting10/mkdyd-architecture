
#ifndef __MKD_GBLDEF_H__
#define __MKD_GBLDEF_H__

#include <signal.h> 

#include "mkd_c_slogic.h"
#include "mkd_c_threadpool.h"

//比较通用的定义和全局变量的外部声明

//结构定义
typedef struct _CConfItem
{
	char ItemName[50];
	char ItemContent[500];
}CConfItem,*LPCConfItem;

//和运行日志相关 
typedef struct
{
	int    log_level;   //日志级别，mkd_macro.h里分0-8共9个级别
	int    fd;          //日志文件描述符

}mkd_log_t;


//外部全局量声明
extern size_t        g_argvneedmem;
extern size_t        g_envneedmem; 
extern int           g_os_argc; 
extern char          **g_os_argv;
extern char          *gp_envmem; 
extern int           g_daemonized;
extern CLogicSocket  g_socket;  
extern CThreadPool   g_threadpool;

extern pid_t         mkd_pid;
extern pid_t         mkd_parent;
extern mkd_log_t     mkd_log;
extern int           mkd_process;   
extern sig_atomic_t  mkd_reap;   
extern int           g_stopEvent;

#endif
