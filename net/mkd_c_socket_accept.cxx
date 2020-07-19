//网络中接受连接【accept】相关
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

//建立新连接专用函数
void CSocekt::mkd_event_accept(lpmkd_connection_t oldc)
{
    struct sockaddr    mysockaddr;        //远端服务器的socket地址
    socklen_t          socklen;
    int                err;
    int                level;
    int                s;
    static int         use_accept4 = 1;   
    lpmkd_connection_t newc;              //代表连接池中的一个连接

    socklen = sizeof(mysockaddr);
    do 
    {     
        if(use_accept4)
        {
            s = accept4(oldc->fd, &mysockaddr, &socklen, SOCK_NONBLOCK); //从内核获取一个用户端连接，最后一个参数SOCK_NONBLOCK表示返回一个非阻塞的socket
        }
        else
        {
            s = accept(oldc->fd, &mysockaddr, &socklen);
        }
        if(s == -1)
        {
            err = errno;

            //对accept、send和recv而言，事件未发生时errno通常被设置成EAGAIN（意为“再来一次”）或者EWOULDBLOCK（意为“期待阻塞”）
            if(err == EAGAIN) 
            {
                return ;
            } 
            level = MKD_LOG_ALERT;
            if (err == ECONNABORTED)  //ECONNRESET错误则发生在对方意外关闭套接字后【您的主机中的软件放弃了一个已建立的连接--由于超时或者其它失败而中止接连(用户插拔网线就可能有这个错误出现)】
            {
                //该错误被描述为“software caused connection abort”，即“软件引起的连接中止”。原因在于当服务和客户进程在完成用于 TCP 连接的“三次握手”后，
                //客户 TCP 却发送了一个 RST （复位）分节，在服务进程看来，就在该连接已由 TCP 排队，等着服务进程调用 accept 的时候 RST 却到达了。
                //POSIX 规定此时的 errno 值必须 ECONNABORTED。源自 Berkeley 的实现完全在内核中处理中止的连接，服务进程将永远不知道该中止的发生。
                //服务器进程一般可以忽略该错误，直接再次调用accept。
                level = MKD_LOG_ERR;
            } 
            else if (err == EMFILE || err == ENFILE) //EMFILE:进程的fd已用尽【已达到系统所允许单一进程所能打开的文件/套接字总数】。可参考：https://blog.csdn.net/sdn_prc/article/details/28661661 以及 https://bbs.csdn.net/topics/390592927
            {
                level = MKD_LOG_CRIT;
            }
            mkd_log_error_core(level,errno,"CSocekt::mkd_event_accept()中accept4()失败!");

            if(use_accept4 && err == ENOSYS) 
            {
                use_accept4 = 0;  
                continue;         
            }

            if (err == ECONNABORTED)  //对方关闭套接字
            {
                //do nothing
            }
            
            if (err == EMFILE || err == ENFILE) 
            {
                //do nothing
            }            
            return;
        } 

        newc = mkd_get_connection(s); //这是针对新连入用户的连接，和监听套接字所对应的连接是两个不同的东西
        if(newc == NULL)
        {
            if(close(s) == -1)
            {
                mkd_log_error_core(MKD_LOG_ALERT,errno,"CSocekt::mkd_event_accept()中close(%d)失败!",s);                
            }
            return;
        }

        //成功的拿到了连接池中的一个连接
        memcpy(&newc->s_sockaddr,&mysockaddr,socklen);  //拷贝客户端地址到连接对象

        if(!use_accept4)
        {
            //如果不是用accept4()取得的socket，那么就要设置为非阻塞【因为用accept4()的已经被accept4()设置为非阻塞了】
            if(setnonblocking(s) == false)
            {
                //设置非阻塞失败
                mkd_close_connection(newc); //关闭socket
                return; 
            }
        }

        newc->listening = oldc->listening;                    //连接对象 和监听对象关联，方便通过连接对象找监听对象【关联到监听端口】
        newc->rhandler = &CSocekt::mkd_read_request_handler;  //设置数据来时的读处理函数
        newc->whandler = &CSocekt::mkd_write_request_handler; //设置数据发送时的写处理函数

        //客户端应该主动发送第一次的数据，将读事件加入epoll监控
         if(mkd_epoll_oper_event(
                                s,                  //socekt句柄
                                EPOLL_CTL_ADD,      //事件类型，这里是增加
                                EPOLLIN|EPOLLRDHUP, //标志，这里代表要增加的标志,EPOLLIN：可读，EPOLLRDHUP：TCP连接的远端关闭或者半关闭 ，如果边缘触发模式可以增加 EPOLLET
                                0,                  //对于事件类型为增加的，不需要这个参数
                                newc                //连接池中的连接
                                ) == -1)         
        {
            mkd_close_connection(newc);
            return; 
        }
        if(m_ifkickTimeCount == 1)
        {
            AddToTimerQueue(newc);
        }
        
        break;  //一般就是循环一次就跳出去
    } while (1);   

    return;
}

