﻿//网络中连接/连接池相关
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>    
#include <stdarg.h>    
#include <unistd.h>    
#include <sys/time.h>  
#include <time.h>      
#include <fcntl.h>     
#include <errno.h>     
#include <sys/ioctl.h> 
#include <arpa/inet.h>

#include "mkd_c_conf.h"
#include "mkd_macro.h"
#include "mkd_global.h"
#include "mkd_func.h"
#include "mkd_c_socket.h"
#include "mkd_c_memory.h"
#include "mkd_c_lockmutex.h"

//---------------------------------------------------------------
//连接池成员函数
mkd_connection_s::mkd_connection_s()//构造函数
{		
    iCurrsequence = 0;    
    pthread_mutex_init(&logicPorcMutex, NULL); //互斥量初始化
}
mkd_connection_s::~mkd_connection_s()//析构函数
{
    pthread_mutex_destroy(&logicPorcMutex);    //互斥量释放
}
//分配出去一个连接的时候初始化
void mkd_connection_s::GetOneToUse()
{
    ++iCurrsequence;

    fd  = -1;                                         //开始先给-1
    curStat = _PKG_HD_INIT;                           //收包状态处于初始状态，准备接收数据包头【状态机】
    precvbuf = dataHeadInfo;                          //收包我要先收到这里来，因为要先收包头，所以收数据的buff直接就是dataHeadInfo
    irecvlen = sizeof(COMM_PKG_HEADER);               //这里指定收数据的长度，这里先要求收包头这么长字节的数据
    
    precvMemPointer = NULL;                           
    iThrowsendCount = 0;                              
    psendMemPointer = NULL;                           //发送数据头指针记录
    events          = 0;                              
    lastPingTime    = time(NULL);                     //上次ping的时间
}

//回收回来一个连接的时候做一些事
void mkd_connection_s::PutOneToFree()
{
    ++iCurrsequence;   
    if(precvMemPointer != NULL)//释放内存
    {        
        CMemory::GetInstance()->FreeMemory(precvMemPointer);
        precvMemPointer = NULL;        
    }
    if(psendMemPointer != NULL) //如果发送数据的缓冲区里有内容，则要释放内存
    {
        CMemory::GetInstance()->FreeMemory(psendMemPointer);
        psendMemPointer = NULL;
    }

    iThrowsendCount = 0;                                    
}

//---------------------------------------------------------------
//初始化连接池
void CSocekt::initconnection()
{
    lpmkd_connection_t p_Conn;
    CMemory *p_memory = CMemory::GetInstance();   

    int ilenconnpool = sizeof(mkd_connection_t);    
    for(int i = 0; i < m_worker_connections; ++i)
    {
        p_Conn = (lpmkd_connection_t)p_memory->AllocMemory(ilenconnpool,true); 
        p_Conn = new(p_Conn) mkd_connection_t();  		
        p_Conn->GetOneToUse();
        m_connectionList.push_back(p_Conn);     //所有连接都放在这个list
        m_freeconnectionList.push_back(p_Conn); //空闲连接会放在这个list
    } 
    m_free_connection_n = m_total_connection_n = m_connectionList.size(); //开始这两个列表一样大
    return;
}

//最终回收连接池，释放内存
void CSocekt::clearconnection()
{
    lpmkd_connection_t p_Conn;
	CMemory *p_memory = CMemory::GetInstance();
	
	while(!m_connectionList.empty())
	{
		p_Conn = m_connectionList.front();
		m_connectionList.pop_front(); 
        p_Conn->~mkd_connection_t();     //手工调用析构函数
		p_memory->FreeMemory(p_Conn);
	}
}

//从连接池中获取一个空闲连接
lpmkd_connection_t CSocekt::mkd_get_connection(int isock)
{
    //因为可能有其他线程要访问m_freeconnectionList，m_connectionList【比如可能有专门的释放线程要释放/或者主线程要释放】之类的，所以应该临界一下
    CLock lock(&m_connectionMutex);  

    if(!m_freeconnectionList.empty())
    {
        //从空闲连接中摘取
        lpmkd_connection_t p_Conn = m_freeconnectionList.front(); //返回第一个元素但不检查元素存在与否
        m_freeconnectionList.pop_front();                         //移除第一个元素但不返回	
        p_Conn->GetOneToUse();
        --m_free_connection_n; 
        p_Conn->fd = isock;
        return p_Conn;
    }

    //走到这里，表示没空闲的连接了，考虑重新创建一个连接
    CMemory *p_memory = CMemory::GetInstance();
    lpmkd_connection_t p_Conn = (lpmkd_connection_t)p_memory->AllocMemory(sizeof(mkd_connection_t),true);
    p_Conn = new(p_Conn) mkd_connection_t();
    p_Conn->GetOneToUse();
    m_connectionList.push_back(p_Conn); //入到总表中来，但不能入到空闲表中来，因为凡是调这个函数的，肯定是要用这个连接的
    ++m_total_connection_n;             
    p_Conn->fd = isock;
    return p_Conn;
}

//归还参数pConn所代表的连接到到连接池中
void CSocekt::mkd_free_connection(lpmkd_connection_t pConn) 
{
    //因为有线程可能要动连接池中连接，所以互斥
    CLock lock(&m_connectionMutex);  

    //释放连接
    pConn->PutOneToFree();

    //扔到空闲连接列表里
    m_freeconnectionList.push_back(pConn);

    //空闲连接数+1
    ++m_free_connection_n;
    return;
}


//将要回收的连接放到一个队列中来，后续有专门的线程会处理这个队列中的连接的回收
//有些连接，不希望马上释放，要隔一段时间后再释放以确保服务器的稳定，所以，我们把这种隔一段时间才释放的连接先放到一个队列中来
void CSocekt::inRecyConnectQueue(lpmkd_connection_t pConn)
{
    std::list<lpmkd_connection_t>::iterator pos;
    bool iffind = false;
        
    CLock lock(&m_recyconnqueueMutex); //针对连接回收列表的互斥量，因为线程ServerRecyConnectionThread()也有要用到这个回收列表

    //如下判断防止连接被多次扔到回收站中来
    for(pos = m_recyconnectionList.begin(); pos != m_recyconnectionList.end(); ++pos)
	{
		if((*pos) == pConn)		
		{	
			iffind = true;
			break;			
		}
	}
    if(iffind == true) 
	{
        return;
    }

    pConn->inRecyTime = time(NULL);        //记录回收时间
    ++pConn->iCurrsequence;
    m_recyconnectionList.push_back(pConn); //等待ServerRecyConnectionThread线程自会处理 
    ++m_totol_recyconnection_n;            //待释放连接队列大小+1
    return;
}

//处理连接回收的线程
void* CSocekt::ServerRecyConnectionThread(void* threadData)
{
    ThreadItem *pThread = static_cast<ThreadItem*>(threadData);
    CSocekt *pSocketObj = pThread->_pThis;
    
    time_t currtime;
    int err;
    std::list<lpmkd_connection_t>::iterator pos,posend;
    lpmkd_connection_t p_Conn;
    
    while(1)
    {
        //为简化问题，直接每次休息200毫秒
        usleep(200 * 1000);  
        //存在待回收连接
        if(pSocketObj->m_totol_recyconnection_n > 0)
        {
            currtime = time(NULL);
            err = pthread_mutex_lock(&pSocketObj->m_recyconnqueueMutex);  
            if(err != 0) mkd_log_stderr(err,"CSocekt::ServerRecyConnectionThread()中pthread_mutex_lock()失败，返回的错误码为%d!",err);

lblRRTD:
            pos    = pSocketObj->m_recyconnectionList.begin();
			posend = pSocketObj->m_recyconnectionList.end();
            for(; pos != posend; ++pos)
            {
                p_Conn = (*pos);
                if(
                    ( (p_Conn->inRecyTime + pSocketObj->m_RecyConnectionWaitTime) > currtime)  && (g_stopEvent == 0) //如果不是要整个系统退出，可以continue，否则就得要强制释放
                    )
                {
                    continue; //没到释放的时间
                }    
                //到释放的时间了: 
                if(p_Conn->iThrowsendCount > 0)
                {
                    mkd_log_stderr(0,"CSocekt::ServerRecyConnectionThread()中到释放时间却发现p_Conn.iThrowsendCount!=0，这个不该发生");
                }

                //流程走到这里，表示可以释放
                --pSocketObj->m_totol_recyconnection_n;        //待释放连接队列大小-1
                pSocketObj->m_recyconnectionList.erase(pos);   //迭代器已经失效，但pos所指内容在p_Conn里保存着呢

                pSocketObj->mkd_free_connection(p_Conn);	   //归还参数pConn所代表的连接到到连接池中
                goto lblRRTD; 
            } 
            err = pthread_mutex_unlock(&pSocketObj->m_recyconnqueueMutex); 
            if(err != 0)  mkd_log_stderr(err,"CSocekt::ServerRecyConnectionThread()pthread_mutex_unlock()失败，返回的错误码为%d!",err);
        } //end if

        if(g_stopEvent == 1) //要退出整个程序，那么肯定要先退出这个循环
        {
            if(pSocketObj->m_totol_recyconnection_n > 0)
            {
                //因为要退出，所以就得硬释放了【不管到没到时间，不管有没有其他不 允许释放的需求，都得硬释放】
                err = pthread_mutex_lock(&pSocketObj->m_recyconnqueueMutex);  
                if(err != 0) mkd_log_stderr(err,"CSocekt::ServerRecyConnectionThread()中pthread_mutex_lock2()失败，返回的错误码为%d!",err);

        lblRRTD2:
                pos    = pSocketObj->m_recyconnectionList.begin();
			    posend = pSocketObj->m_recyconnectionList.end();
                for(; pos != posend; ++pos)
                {
                    p_Conn = (*pos);
                    --pSocketObj->m_totol_recyconnection_n;        //待释放连接队列大小-1
                    pSocketObj->m_recyconnectionList.erase(pos);   //迭代器已经失效，但pos所指内容在p_Conn里保存着呢
                    pSocketObj->mkd_free_connection(p_Conn);	   //归还参数pConn所代表的连接到到连接池中
                    goto lblRRTD2; 
                } 
                err = pthread_mutex_unlock(&pSocketObj->m_recyconnqueueMutex); 
                if(err != 0)  mkd_log_stderr(err,"CSocekt::ServerRecyConnectionThread()pthread_mutex_unlock2()失败，返回的错误码为%d!",err);
            }
            break; 
        }  
    }    
    
    return (void*)0;
}

void CSocekt::mkd_close_connection(lpmkd_connection_t pConn)
{    
    mkd_free_connection(pConn); 
    if(pConn->fd != -1)
    {
        close(pConn->fd);
        pConn->fd = -1;
    }    
    return;
}
