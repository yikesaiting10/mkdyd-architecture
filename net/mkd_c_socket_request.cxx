//网络中客户端发送来数据/服务器端收包相关
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
#include <pthread.h>   

#include "mkd_c_conf.h"
#include "mkd_macro.h"
#include "mkd_global.h"
#include "mkd_func.h"
#include "mkd_c_socket.h"
#include "mkd_c_memory.h"
#include "mkd_c_lockmutex.h"  //自动释放互斥量的一个类

//来数据时候的处理
void CSocekt::mkd_read_request_handler(lpmkd_connection_t pConn)
{  
    //收包
    ssize_t reco = recvproc(pConn,pConn->precvbuf,pConn->irecvlen); 
    if(reco <= 0)  
    {
        return;      
    }

    //走到这里，说明成功收到了一些字节 
    if(pConn->curStat == _PKG_HD_INIT) 
    {        
        if(reco == m_iLenPkgHeader)//收到完整包头，拆解包头
        {   
            mkd_wait_request_handler_proc_p1(pConn); //调用专门针对包头处理完整的函数
        }
        else
		{
			//收到的包头不完整
            pConn->curStat        = _PKG_HD_RECVING;                 //接收包头中，包头不完整，继续接收包头中	
            pConn->precvbuf       = pConn->precvbuf + reco;              //收后续包的内存往后走
            pConn->irecvlen       = pConn->irecvlen - reco;              //要收的内容当然要减少，以确保只收到完整的包头先
        } 
    } 
    else if(pConn->curStat == _PKG_HD_RECVING) //接收包头中，包头不完整，继续接收中，这个条件才会成立
    {
        if(pConn->irecvlen == reco) //要求收到的宽度和实际收到的宽度相等
        {
            //包头收完整了
            mkd_wait_request_handler_proc_p1(pConn); //调用专门针对包头处理完整的函数
        }
        else
		{
			//包头还是没收完整，继续收包头
            //pConn->curStat        = _PKG_HD_RECVING;                 
            pConn->precvbuf       = pConn->precvbuf + reco;              //注意收后续包的内存往后走
            pConn->irecvlen       = pConn->irecvlen - reco;              //要收的内容当然要减少，以确保只收到完整的包头先
        }
    }
    else if(pConn->curStat == _PKG_BD_INIT) 
    {
        //包头好收完，准备接收包体
        if(reco == pConn->irecvlen)
        {
            //收到的宽度等于要收的宽度，包体也收完整，进行处理
            mkd_wait_request_handler_proc_plast(pConn);
        }
        else
		{
			//收到的宽度小于要收的宽度
			pConn->curStat = _PKG_BD_RECVING;					
			pConn->precvbuf = pConn->precvbuf + reco;
			pConn->irecvlen = pConn->irecvlen - reco;
		}
    }
    else if(pConn->curStat == _PKG_BD_RECVING) 
    {
        //接收包体中，包体不完整，继续接收中
        if(pConn->irecvlen == reco)
        {
            //包体收完整了，进行处理
            mkd_wait_request_handler_proc_plast(pConn);
        }
        else
        {
            //包体没收完整，继续收
            pConn->precvbuf = pConn->precvbuf + reco;
			pConn->irecvlen = pConn->irecvlen - reco;
        }
    }
    return;
}

//接收数据专用函数
//参数c：连接池中相关连接
//参数buff：接收数据的缓冲区
//参数buflen：要接收的数据大小
ssize_t CSocekt::recvproc(lpmkd_connection_t c,char *buff,ssize_t buflen) 
{
    ssize_t n;
    
    n = recv(c->fd, buff, buflen, 0); //recv()系统函数， 最后一个参数flag，一般为0     
    if(n == 0)
    {
        //客户端关闭，直接回收连接，关闭socket即可 
        zdClosesocketProc(c);        
        return -1;
    }
    //客户端没断，走这里 
    if(n < 0) 
    {
        if(errno == EAGAIN || errno == EWOULDBLOCK)
        {
            mkd_log_stderr(errno,"CSocekt::recvproc()中errno == EAGAIN || errno == EWOULDBLOCK成立，出乎我意料！");
            return -1; 
        }
        if(errno == EINTR)  
        {
            mkd_log_stderr(errno,"CSocekt::recvproc()中errno == EINTR成立，出乎我意料！");
            return -1;
        }

        if(errno == ECONNRESET)  
        {
            //do nothing
        }
        else
        {
            mkd_log_stderr(errno,"CSocekt::recvproc()中发生错误，我打印出来看看是啥错误！"); 
        } 
        zdClosesocketProc(c);
        return -1;
    }
    return n; //返回收到的字节数
}


//包头收完整后的处理，记为包处理阶段1【p1】
void CSocekt::mkd_wait_request_handler_proc_p1(lpmkd_connection_t pConn)
{
    CMemory *p_memory = CMemory::GetInstance();		

    LPCOMM_PKG_HEADER pPkgHeader;
    pPkgHeader = (LPCOMM_PKG_HEADER)pConn->dataHeadInfo; 

    unsigned short e_pkgLen; 
    e_pkgLen = ntohs(pPkgHeader->pkgLen);  
    //恶意包或者错误包的判断
    if(e_pkgLen < m_iLenPkgHeader) 
    {
        //伪造包/或者包错误
        pConn->curStat = _PKG_HD_INIT;      
        pConn->precvbuf = pConn->dataHeadInfo;
        pConn->irecvlen = m_iLenPkgHeader;
    }
    else if(e_pkgLen > (_PKG_MAX_LENGTH-1000))  
    {
        //恶意包，太大，认定非法用户，
        pConn->curStat = _PKG_HD_INIT;
        pConn->precvbuf = pConn->dataHeadInfo;
        pConn->irecvlen = m_iLenPkgHeader;
    }
    else
    {
        //合法的包头，继续处理
        char *pTmpBuffer  = (char *)p_memory->AllocMemory(m_iLenMsgHeader + e_pkgLen,false); //分配内存【长度是 消息头长度  + 包头长度 + 包体长度】
        pConn->precvMemPointer = pTmpBuffer;  //内存开始指针

        //先填写消息头内容
        LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)pTmpBuffer;
        ptmpMsgHeader->pConn = pConn;
        ptmpMsgHeader->iCurrsequence = pConn->iCurrsequence; //收到包时的连接池中连接序号记录到消息头里来
        //再填写包头内容
        pTmpBuffer += m_iLenMsgHeader;                 //往后跳，跳过消息头，指向包头
        memcpy(pTmpBuffer,pPkgHeader,m_iLenPkgHeader); //直接把收到的包头拷贝进来
        if(e_pkgLen == m_iLenPkgHeader)
        {
            //该报文只有包头无包体，则直接入消息队列待后续业务逻辑线程去处理
            mkd_wait_request_handler_proc_plast(pConn);
        } 
        else
        {
            //开始收包体
            pConn->curStat = _PKG_BD_INIT;                   //当前状态发生改变，包头刚好收完，准备接收包体	    
            pConn->precvbuf = pTmpBuffer + m_iLenPkgHeader;  //pTmpBuffer指向包头
            pConn->irecvlen = e_pkgLen - m_iLenPkgHeader;    //e_pkgLen是整个包【包头+包体】大小
        }                       
    }  

    return;
}

//收到一个完整包后的处理【plast表示最后阶段】
void CSocekt::mkd_wait_request_handler_proc_plast(lpmkd_connection_t pConn)
{
    //把这段内存放到消息队列中来；
    g_threadpool.inMsgRecvQueueAndSignal(pConn->precvMemPointer); //入消息队列并触发线程处理消息
    
    pConn->precvMemPointer = NULL;
    pConn->curStat         = _PKG_HD_INIT;     //收包状态机的状态恢复为原始态，为收下一个包做准备                    
    pConn->precvbuf        = pConn->dataHeadInfo;  //设置好收包的位置
    pConn->irecvlen        = m_iLenPkgHeader;  //设置好要接收数据的大小
    return;
}

//发送数据专用函数，返回本次发送的字节数
ssize_t CSocekt::sendproc(lpmkd_connection_t c,char *buff,ssize_t size)  
{
    ssize_t n;

    for ( ;; )
    {
        n = send(c->fd, buff, size, 0); //send()系统函数， 最后一个参数flag，一般为0；
        if(n > 0) 
        {
            return n; //返回本次发送的字节数
        }

        if(n == 0)
        {
            //网上找资料：send=0表示超时，对方主动关闭了连接过程
            return 0;
        }

        if(errno == EAGAIN)  //等于EWOULDBLOCK
        {
            return -1;  //表示发送缓冲区满了
        }

        if(errno == EINTR) 
        {
            mkd_log_stderr(errno,"CSocekt::sendproc()中send()失败.");  
        }
        else
        {
            return -2;    
        }
    } 
}

//设置数据发送时的写处理函数,当数据可写时epoll通知
//能走到这里，数据就是没法送完毕，要继续发送
void CSocekt::mkd_write_request_handler(lpmkd_connection_t pConn)
{      
    CMemory *p_memory = CMemory::GetInstance();

    ssize_t sendsize = sendproc(pConn,pConn->psendbuf,pConn->isendlen);

    if(sendsize > 0 && sendsize != pConn->isendlen)
    {        
        pConn->psendbuf = pConn->psendbuf + sendsize;
		pConn->isendlen = pConn->isendlen - sendsize;	
        return;
    }
    else if(sendsize == -1)
    {
        mkd_log_stderr(errno,"CSocekt::mkd_write_request_handler()时if(sendsize == -1)成立，这很怪异。"); 
        return;
    }

    if(sendsize > 0 && sendsize == pConn->isendlen) 
    {
        //如果是成功的发送完毕数据，则把写事件通知从epoll中干掉
        if(mkd_epoll_oper_event(
                pConn->fd,          //socket句柄
                EPOLL_CTL_MOD,      //事件类型，这里是修改
                EPOLLOUT,           //标志，这里代表要减去的标志,EPOLLOUT：可写
                1,                  //对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数, 0：增加   1：去掉 2：完全覆盖
                pConn               //连接池中的连接
                ) == -1)
        {
            mkd_log_stderr(errno,"CSocekt::mkd_write_request_handler()中mkd_epoll_oper_event()失败。");
        }    
    }

    if(sem_post(&m_semEventSendQueue)==-1)       
        mkd_log_stderr(0,"CSocekt::mkd_write_request_handler()中sem_post(&m_semEventSendQueue)失败.");


    p_memory->FreeMemory(pConn->psendMemPointer);  //释放内存
    pConn->psendMemPointer = NULL;        
    --pConn->iThrowsendCount;  
    return;
}

//消息处理线程主函数，专门处理各种接收到的TCP消息
//pMsgBuf：发送过来的消息缓冲区，消息本身是自解释的，通过包头可以计算整个包长
void CSocekt::threadRecvProcFunc(char *pMsgBuf)
{   
    return;
}


