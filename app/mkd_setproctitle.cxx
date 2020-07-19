//设置课执行程序标题相关 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>  //env
#include <string.h>

#include "mkd_global.h"

//设置可执行程序标题相关函数：分配内存，并且把环境变量拷贝到新内存中来
void mkd_init_setproctitle()
{    
    gp_envmem = new char[g_envneedmem]; 
    memset(gp_envmem,0,g_envneedmem);  
    char *ptmp = gp_envmem;
    for (int i = 0; environ[i]; i++) 
    {
        size_t size = strlen(environ[i])+1 ; 
        strcpy(ptmp,environ[i]);      
        environ[i] = ptmp;            
        ptmp += size;
    }
    return;
}

//设置可执行程序标题
void mkd_setproctitle(const char *title)
{
    size_t ititlelen = strlen(title);   
    size_t esy = g_argvneedmem + g_envneedmem; 
    if( esy <= ititlelen)
    {
        return;
    }
    g_os_argv[1] = NULL;  
    char *ptmp = g_os_argv[0]; 
    strcpy(ptmp,title);
    ptmp += ititlelen; 
    size_t cha = esy - ititlelen; 
    memset(ptmp,0,cha);
    return;
}