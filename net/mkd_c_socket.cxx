//和网络相关
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

//--------------------------------------------------------------------------
//构造函数
CSocekt::CSocekt()
{
    //配置相关
    m_worker_connections = 1;      //epoll连接最大项数
    m_ListenPortCount = 1;         //监听一个端口
    m_RecyConnectionWaitTime = 60; //等待这么些秒后才回收连接

    //epoll相关
    m_epollhandle = -1;          //epoll返回的句柄

    //一些和网络通讯有关的常用变量值
    m_iLenPkgHeader = sizeof(COMM_PKG_HEADER);    //包头的sizeof值【占用的字节数】
    m_iLenMsgHeader = sizeof(STRUC_MSG_HEADER);  //消息头的sizeof值【占用的字节数】 

    //各种队列相关
    m_iSendMsgQueueCount     = 0;     //发消息队列大小
    m_totol_recyconnection_n = 0;     //待释放连接队列大小
    m_cur_size_              = 0;     //当前计时队列尺寸
    m_timer_value_           = 0;     //当前计时队列头部的时间值
    return;	
}

//初始化函数
bool CSocekt::Initialize()
{
    ReadConf();  //读配置项
    if(mkd_open_listening_sockets() == false)  //打开监听端口    
        return false;  
    return true;
}

//子进程中需要执行的初始化函数
bool CSocekt::Initialize_subproc()
{
    //发消息互斥量初始化
    if(pthread_mutex_init(&m_sendMessageQueueMutex, NULL)  != 0)
    {        
        mkd_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_mutex_init(&m_sendMessageQueueMutex)失败.");
        return false;    
    }
    //连接相关互斥量初始化
    if(pthread_mutex_init(&m_connectionMutex, NULL)  != 0)
    {
        mkd_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_mutex_init(&m_connectionMutex)失败.");
        return false;    
    }    
    //连接回收队列相关互斥量初始化
    if(pthread_mutex_init(&m_recyconnqueueMutex, NULL)  != 0)
    {
        mkd_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_mutex_init(&m_recyconnqueueMutex)失败.");
        return false;    
    } 
    //和时间处理队列有关的互斥量初始化
    if(pthread_mutex_init(&m_timequeueMutex, NULL)  != 0)
    {
        mkd_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_mutex_init(&m_timequeueMutex)失败.");
        return false;    
    }
   
    //初始化发消息相关信号量，信号量用于进程/线程 之间的同步
    //第二个参数=0，表示信号量在线程之间共享，确实如此 ，如果非0，表示在进程之间共享
    //第三个参数=0，表示信号量的初始值，为0时，调用sem_wait()就会卡在那里卡着
    if(sem_init(&m_semEventSendQueue,0,0) == -1)
    {
        mkd_log_stderr(0,"CSocekt::Initialize_subproc()中sem_init(&m_semEventSendQueue,0,0)失败.");
        return false;
    }

    //创建线程
    int err;
    ThreadItem *pSendQueue;    //专门用来发送数据的线程
    m_threadVector.push_back(pSendQueue = new ThreadItem(this));                         //创建一个新线程对象并入到容器中 
    err = pthread_create(&pSendQueue->_Handle, NULL, ServerSendQueueThread,pSendQueue); //创建线程
    if(err != 0)
    {
        mkd_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_create(ServerSendQueueThread)失败.");
        return false;
    }

    ThreadItem *pRecyconn;    //专门用来回收连接的线程
    m_threadVector.push_back(pRecyconn = new ThreadItem(this)); 
    err = pthread_create(&pRecyconn->_Handle, NULL, ServerRecyConnectionThread,pRecyconn);
    if(err != 0)
    {
        mkd_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_create(ServerRecyConnectionThread)失败.");
        return false;
    }

    if(m_ifkickTimeCount == 1)  //是否开启踢人时钟，1：开启   0：不开启
    {
        ThreadItem *pTimemonitor;    //专门用来处理到期不发心跳包的用户踢出的线程
        m_threadVector.push_back(pTimemonitor = new ThreadItem(this)); 
        err = pthread_create(&pTimemonitor->_Handle, NULL, ServerTimerQueueMonitorThread,pTimemonitor);
        if(err != 0)
        {
            mkd_log_stderr(0,"CSocekt::Initialize_subproc()中pthread_create(ServerTimerQueueMonitorThread)失败.");
            return false;
        }
    }

    return true;
}

//--------------------------------------------------------------------------
//释放函数
CSocekt::~CSocekt()
{
    //释放必须的内存
    //监听端口相关内存的释放--------
    std::vector<lpmkd_listening_t>::iterator pos;	
	for(pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos) 
	{		
		delete (*pos); 
	}
	m_ListenSocketList.clear();    
    return;
}

//关闭退出函数[子进程中执行]
void CSocekt::Shutdown_subproc()
{
    //把干活的线程停止掉
    //用到信号量的还需要调用一下sem_post
    if(sem_post(&m_semEventSendQueue)==-1)  //让ServerSendQueueThread()流程走下来干活
    {
         mkd_log_stderr(0,"CSocekt::Shutdown_subproc()中sem_post(&m_semEventSendQueue)失败.");
    }

    std::vector<ThreadItem*>::iterator iter;
	for(iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        pthread_join((*iter)->_Handle, NULL); //等待一个线程终止
    }
    //释放一下new出来的ThreadItem【线程池中的线程】    
	for(iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
	{
		if(*iter)
			delete *iter;
	}
	m_threadVector.clear();

    //队列相关
    clearMsgSendQueue();
    clearconnection();
    clearAllFromTimerQueue();
    
    //多线程相关    
    pthread_mutex_destroy(&m_connectionMutex);          //连接相关互斥量释放
    pthread_mutex_destroy(&m_sendMessageQueueMutex);    //发消息互斥量释放    
    pthread_mutex_destroy(&m_recyconnqueueMutex);       //连接回收队列相关的互斥量释放
    pthread_mutex_destroy(&m_timequeueMutex);           //时间处理队列相关的互斥量释放
    sem_destroy(&m_semEventSendQueue);                  //发消息相关线程信号量释放
}

//清理TCP发送消息队列
void CSocekt::clearMsgSendQueue()
{
	char * sTmpMempoint;
	CMemory *p_memory = CMemory::GetInstance();
	
	while(!m_MsgSendQueue.empty())
	{
		sTmpMempoint = m_MsgSendQueue.front();
		m_MsgSendQueue.pop_front(); 
		p_memory->FreeMemory(sTmpMempoint);
	}	
}

//专门用于读各种配置项
void CSocekt::ReadConf()
{
    CConfig *p_config = CConfig::GetInstance();
    m_worker_connections      = p_config->GetIntDefault("worker_connections",m_worker_connections);              //epoll连接的最大项数
    m_ListenPortCount         = p_config->GetIntDefault("ListenPortCount",m_ListenPortCount);                    //取得要监听的端口数量
    m_RecyConnectionWaitTime  = p_config->GetIntDefault("Sock_RecyConnectionWaitTime",m_RecyConnectionWaitTime); //等待这么些秒后才回收连接

    m_ifkickTimeCount         = p_config->GetIntDefault("Sock_WaitTimeEnable",0);                                //是否开启踢人时钟，1：开启   0：不开启
	m_iWaitTime               = p_config->GetIntDefault("Sock_MaxWaitTime",m_iWaitTime);                         //多少秒检测一次是否 心跳超时，只有当Sock_WaitTimeEnable = 1时，本项才有用	
	m_iWaitTime               = (m_iWaitTime > 5)?m_iWaitTime:5;                                                 //不建议低于5秒钟，因为无需太频繁

    return;
}

//监听端口【支持多个端口】
//在创建worker进程之前就要执行这个函数
bool CSocekt::mkd_open_listening_sockets()
{    
    int                isock;                //socket
    struct sockaddr_in serv_addr;            //服务器的地址结构体
    int                iport;                //端口
    char               strinfo[100];         //临时字符串 
   
    //初始化相关
    memset(&serv_addr,0,sizeof(serv_addr));  
    serv_addr.sin_family = AF_INET;                //选择协议族为IPV4
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); //监听本地所有的IP地址；INADDR_ANY表示的是一个服务器上所有的网卡（服务器可能不止一个网卡）多个本地ip地址都进行绑定端口号，进行侦听。

    CConfig *p_config = CConfig::GetInstance();
    for(int i = 0; i < m_ListenPortCount; i++) 
    {        
        //参数1：AF_INET：使用ipv4协议
        //参数2：SOCK_STREAM：使用TCP，表示可靠连接
        //参数3：给0，固定用法
        isock = socket(AF_INET,SOCK_STREAM,0); //系统函数，成功返回非负描述符，出错返回-1
        if(isock == -1)
        {
            mkd_log_stderr(errno,"CSocekt::Initialize()中socket()失败,i=%d.",i);
            return false;
        }

        //setsockopt（）:设置一些套接字参数选项
        //参数2：是表示级别，和参数3配套使用
        //参数3：允许重用本地地址
        //设置 SO_REUSEADDR，主要是解决TIME_WAIT这个状态导致bind()失败的问题
        int reuseaddr = 1; 
        if(setsockopt(isock,SOL_SOCKET, SO_REUSEADDR,(const void *) &reuseaddr, sizeof(reuseaddr)) == -1)
        {
            mkd_log_stderr(errno,"CSocekt::Initialize()中setsockopt(SO_REUSEADDR)失败,i=%d.",i);
            close(isock);                                                
            return false;
        }
        //设置该socket为非阻塞
        if(setnonblocking(isock) == false)
        {                
            mkd_log_stderr(errno,"CSocekt::Initialize()中setnonblocking()失败,i=%d.",i);
            close(isock);
            return false;
        }

        //设置本服务器要监听的地址和端口    
        strinfo[0] = 0;
        sprintf(strinfo,"ListenPort%d",i);
        iport = p_config->GetIntDefault(strinfo,10000);
        serv_addr.sin_port = htons((in_port_t)iport); 

        //绑定服务器地址结构体
        if(bind(isock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
        {
            mkd_log_stderr(errno,"CSocekt::Initialize()中bind()失败,i=%d.",i);
            close(isock);
            return false;
        }
        
        //开始监听
        if(listen(isock,MKD_LISTEN_BACKLOG) == -1)
        {
            mkd_log_stderr(errno,"CSocekt::Initialize()中listen()失败,i=%d.",i);
            close(isock);
            return false;
        }

        lpmkd_listening_t p_listensocketitem = new mkd_listening_t; 
        memset(p_listensocketitem,0,sizeof(mkd_listening_t));      
        p_listensocketitem->port = iport;                          //记录下所监听的端口号
        p_listensocketitem->fd   = isock;                          //套接字木柄保存下来   
        mkd_log_error_core(MKD_LOG_INFO,0,"监听%d端口成功!",iport); //显示一些信息到日志中
        m_ListenSocketList.push_back(p_listensocketitem);          //加入到队列中
    }     
    if(m_ListenSocketList.size() <= 0) 
        return false;
    return true;
}

//设置socket连接为非阻塞模式
bool CSocekt::setnonblocking(int sockfd) 
{    
    int nb = 1; //0：清除，1：设置  
    if(ioctl(sockfd, FIONBIO, &nb) == -1) //FIONBIO：设置/清除非阻塞I/O标记：0：清除，1：设置
    {
        return false;
    }
    return true;
}

//关闭socket
void CSocekt::mkd_close_listening_sockets()
{
    for(int i = 0; i < m_ListenPortCount; i++) 
    {  
        close(m_ListenSocketList[i]->fd);
        mkd_log_error_core(MKD_LOG_INFO,0,"关闭监听端口%d!",m_ListenSocketList[i]->port); 
    }
    return;
}

//将一个待发送消息入到发消息队列中
void CSocekt::msgSend(char *psendbuf) 
{
    CLock lock(&m_sendMessageQueueMutex);  //互斥量
    m_MsgSendQueue.push_back(psendbuf);    
    ++m_iSendMsgQueueCount;   //原子操作

    //将信号量的值+1,这样其他卡在sem_wait的就可以走下去
    if(sem_post(&m_semEventSendQueue)==-1)  //让ServerSendQueueThread()流程走下来干活
    {
         mkd_log_stderr(0,"CSocekt::msgSend()中sem_post(&m_semEventSendQueue)失败.");      
    }
    return;
}

//主动关闭一个连接时的要做些善后的处理函数
void CSocekt::zdClosesocketProc(lpmkd_connection_t p_Conn)
{
    if(m_ifkickTimeCount == 1)
    {
        DeleteFromTimerQueue(p_Conn); //从时间队列中把连接干掉
    }
    if(p_Conn->fd != -1)
    {   
        close(p_Conn->fd); //这个socket关闭，关闭后epoll就会被从红黑树中删除，所以这之后无法收到任何epoll事件
        p_Conn->fd = -1;
    }

    if(p_Conn->iThrowsendCount > 0)  
        --p_Conn->iThrowsendCount;   

    inRecyConnectQueue(p_Conn);
    return;
}

//--------------------------------------------------------------------
//epoll功能初始化，子进程中进行 
int CSocekt::mkd_epoll_init()
{
    //创建一个epoll对象，创建了一个红黑树，还创建了一个双向链表
    m_epollhandle = epoll_create(m_worker_connections);    
    if (m_epollhandle == -1) 
    {
        mkd_log_stderr(errno,"CSocekt::mkd_epoll_init()中epoll_create()失败.");
        exit(2); 
    }

    //创建连接池数组
    initconnection();
    
    //遍历所有监听socket【监听端口】，我们为每个监听socket增加一个连接池中的连接
    std::vector<lpmkd_listening_t>::iterator pos;	
	for(pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        lpmkd_connection_t p_Conn = mkd_get_connection((*pos)->fd); //从连接池中获取一个空闲连接对象
        if (p_Conn == NULL)
        {
            mkd_log_stderr(errno,"CSocekt::mkd_epoll_init()中mkd_get_connection()失败.");
            exit(2);
        }
        p_Conn->listening = (*pos);   //连接对象 和监听对象关联，方便通过连接对象找监听对象
        (*pos)->connection = p_Conn;  //监听对象 和连接对象关联，方便通过监听对象找连接对象

        //对监听端口的读事件设置处理方法，因为监听端口是用来等对方连接的发送三路握手的，所以监听端口关心的就是读事件
        p_Conn->rhandler = &CSocekt::mkd_event_accept;

        //往监听socket上增加监听事件，从而开始让监听端口履行其职责
        if(mkd_epoll_oper_event(
                                (*pos)->fd,         //socekt句柄
                                EPOLL_CTL_ADD,      //事件类型，这里是增加
                                EPOLLIN|EPOLLRDHUP, //标志，这里代表要增加的标志,EPOLLIN：可读，EPOLLRDHUP：TCP连接的远端关闭或者半关闭
                                0,                  //对于事件类型为增加的，不需要这个参数
                                p_Conn              //连接池中的连接 
                                ) == -1) 
        {
            exit(2); 
        }
    }
    return 1;
}

//对epoll事件的具体操作
int CSocekt::mkd_epoll_oper_event(
                        int                fd,               //句柄，一个socket
                        uint32_t           eventtype,        //事件类型，一般是EPOLL_CTL_ADD，EPOLL_CTL_MOD，EPOLL_CTL_DEL
                        uint32_t           flag,             //标志，具体含义取决于eventtype
                        int                bcaction,         //补充动作，用于补充flag标记的不足  :  0：增加   1：去掉 2：完全覆盖
                        lpmkd_connection_t pConn             //pConn：一个连接，EPOLL_CTL_ADD时增加到红黑树中去，将来epoll_wait时能取出来用
                        )
{
    struct epoll_event ev;    
    memset(&ev, 0, sizeof(ev));

    if(eventtype == EPOLL_CTL_ADD) //往红黑树中增加节点；
    {
        ev.events = flag;      
        pConn->events = flag;  
    }
    else if(eventtype == EPOLL_CTL_MOD)
    {
        //节点已经在红黑树中，修改节点的事件信息
        ev.events = pConn->events;  //先把标记恢复回来
        if(bcaction == 0)
        {
            //增加某个标记            
            ev.events |= flag;
        }
        else if(bcaction == 1)
        {
            //去掉某个标记
            ev.events &= ~flag;
        }
        else
        {
            //完全覆盖某个标记            
            ev.events = flag;      //完全覆盖            
        }
        pConn->events = ev.events; //记录该标记
    }
    else
    {
        //删除红黑树中节点
        return  1;  //先直接返回1表示成功
    } 

    ev.data.ptr = (void *)pConn;

    if(epoll_ctl(m_epollhandle,eventtype,fd,&ev) == -1)
    {
        mkd_log_stderr(errno,"CSocekt::mkd_epoll_oper_event()中epoll_ctl(%d,%ud,%ud,%d)失败.",fd,eventtype,flag,bcaction);    
        return -1;
    }
    return 1;
}

//开始获取发生的事件消息
int CSocekt::mkd_epoll_process_events(int timer) 
{   
    int events = epoll_wait(m_epollhandle,m_events,MKD_MAX_EVENTS,timer);
    
    if(events == -1)
    {
        if(errno == EINTR) 
        {
            mkd_log_error_core(MKD_LOG_INFO,errno,"CSocekt::mkd_epoll_process_events()中epoll_wait()失败!"); 
            return 1;  
        }
        else
        {
            mkd_log_error_core(MKD_LOG_ALERT,errno,"CSocekt::mkd_epoll_process_events()中epoll_wait()失败!"); 
            return 0;  
        }
    }

    if(events == 0)
    {
        if(timer != -1)
        {
            return 1;
        }       
        mkd_log_error_core(MKD_LOG_ALERT,0,"CSocekt::mkd_epoll_process_events()中epoll_wait()没超时却没返回任何事件!"); 
        return 0; 
    }

    //走到这里，就是属于有事件收到了
    lpmkd_connection_t p_Conn;
    uint32_t           revents;
    for(int i = 0; i < events; ++i)    //遍历本次epoll_wait返回的所有事件
    {
        p_Conn = (lpmkd_connection_t)(m_events[i].data.ptr);          

        revents = m_events[i].events;//取出事件类型

        if(revents & EPOLLIN)  //如果是读事件
        {           
            (this->* (p_Conn->rhandler) )(p_Conn);    //注意括号的运用来正确设置优先级，防止编译出错
                                                      //如果新连接进入，这里执行的应该是CSocekt::mkd_event_accept(c)           
                                                      //如果是已经连入，发送数据到这里，则这里执行的应该是 CSocekt::mkd_read_request_handler()            
        }
        
        if(revents & EPOLLOUT) //如果是写事件
        {
            if(revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) //客户端关闭
            {
                --p_Conn->iThrowsendCount;                 
            }
            else
            {
                (this->* (p_Conn->whandler) )(p_Conn);   //如果有数据没有发送完毕，由系统驱动来发送，则这里执行的应该是 CSocekt::mkd_write_request_handler()
            }
            
        }
    } 
    return 1;
}

//--------------------------------------------------------------------
//处理发送消息队列的线程
void* CSocekt::ServerSendQueueThread(void* threadData)
{    
    ThreadItem *pThread = static_cast<ThreadItem*>(threadData);
    CSocekt *pSocketObj = pThread->_pThis;
    int err;
    std::list <char *>::iterator pos,pos2,posend;
    
    char *pMsgBuf;	
    LPSTRUC_MSG_HEADER	pMsgHeader;
	LPCOMM_PKG_HEADER   pPkgHeader;
    lpmkd_connection_t  p_Conn;
    unsigned short      itmp;
    ssize_t             sendsize;  

    CMemory *p_memory = CMemory::GetInstance();
    
    while(g_stopEvent == 0)
    {
        if(sem_wait(&pSocketObj->m_semEventSendQueue) == -1)
        {
            if(errno != EINTR) 
                mkd_log_stderr(errno,"CSocekt::ServerSendQueueThread()中sem_wait(&pSocketObj->m_semEventSendQueue)失败.");            
        }

        //一般走到这里都表示需要处理数据收发了
        if(g_stopEvent != 0)  //要求整个进程退出
            break;

        if(pSocketObj->m_iSendMsgQueueCount > 0) 
        {
            err = pthread_mutex_lock(&pSocketObj->m_sendMessageQueueMutex); //操作发送消息队列m_MsgSendQueue，所以这里要临界            
            if(err != 0) mkd_log_stderr(err,"CSocekt::ServerSendQueueThread()中pthread_mutex_lock()失败，返回的错误码为%d!",err);

            pos    = pSocketObj->m_MsgSendQueue.begin();
			posend = pSocketObj->m_MsgSendQueue.end();

            while(pos != posend)
            {
                pMsgBuf = (*pos);                          //拿到的每个消息都是 消息头+包头+包体
                pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;  //指向消息头
                pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf+pSocketObj->m_iLenMsgHeader);	//指向包头
                p_Conn = pMsgHeader->pConn;

                //包过期，因为如果这个连接被回收，比如在mkd_close_connection(),inRecyConnectQueue()中都会自增iCurrsequence，要发送的数据不需要发送了
                if(p_Conn->iCurrsequence != pMsgHeader->iCurrsequence) 
                {
                    pos2=pos;
                    pos++;
                    pSocketObj->m_MsgSendQueue.erase(pos2);
                    --pSocketObj->m_iSendMsgQueueCount; //发送消息队列容量少1		
                    p_memory->FreeMemory(pMsgBuf);	
                    continue;
                } 

                if(p_Conn->iThrowsendCount > 0) 
                {
                    pos++;
                    continue;
                }
            
                //走到这里，可以发送消息
                p_Conn->psendMemPointer = pMsgBuf;      
                pos2=pos;
				pos++;
                pSocketObj->m_MsgSendQueue.erase(pos2);
                --pSocketObj->m_iSendMsgQueueCount;      	
                p_Conn->psendbuf = (char *)pPkgHeader;   
                itmp = ntohs(pPkgHeader->pkgLen);        
                p_Conn->isendlen = itmp;                 
                                
                //epoll水平触发发送数据的改进方案：
	            //开始不把socket写事件通知加入到epoll,当需要写数据的时候，直接调用write/send发送数据；
	            //如果返回了EAGIN【发送缓冲区满了，需要等待可写事件才能继续往缓冲区里写数据】，此时，再把写事件通知加入到epoll，
	            //此时，就变成了在epoll驱动下写数据，全部数据发送完毕后，再把写事件通知从epoll中干掉
	            //优点：数据不多的时候，可以避免epoll的写事件的增加/删除，提高了程序的执行效率；                         
                //直接调用write或者send发送数据
                sendsize = pSocketObj->sendproc(p_Conn,p_Conn->psendbuf,p_Conn->isendlen); 
                if(sendsize > 0)
                {                    
                    if(sendsize == p_Conn->isendlen) //成功发送出去了数据
                    {
                        //成功发送的和要求发送的数据相等，说明全部发送成功了 发送缓冲区去了【数据全部发完】
                        p_memory->FreeMemory(p_Conn->psendMemPointer);  //释放内存
                        p_Conn->psendMemPointer = NULL;
                        p_Conn->iThrowsendCount = 0;  
                    }
                    else  //没有全部发送完毕(EAGAIN)，数据只发出去了一部分，肯定是因为发送缓冲区满了
                    {                        
                        //发送到了哪里，剩余多少，记录下来，方便下次sendproc()时使用
                        p_Conn->psendbuf = p_Conn->psendbuf + sendsize;
				        p_Conn->isendlen = p_Conn->isendlen - sendsize;	
                        //因为发送缓冲区慢了，所以现在要依赖系统通知来发送数据
                        ++p_Conn->iThrowsendCount;             //标记发送缓冲区满了
                        //投递此事件后，依靠epoll驱动调用mkd_write_request_handler()函数发送数据
                        if(pSocketObj->mkd_epoll_oper_event(
                                p_Conn->fd,         //socket句柄
                                EPOLL_CTL_MOD,      //事件类型，这里是增加【因为我们准备增加个写通知】
                                EPOLLOUT,           //标志，这里代表要增加的标志,EPOLLOUT：可写【可写的时候通知我】
                                0,                  //对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数, 0：增加   1：去掉 2：完全覆盖
                                p_Conn              //连接池中的连接
                                ) == -1)
                        {
                            mkd_log_stderr(errno,"CSocekt::ServerSendQueueThread()mkd_epoll_oper_event()失败.");
                        }
                    } 
                    continue;                     
                } 
                else if(sendsize == 0)
                {
                    p_memory->FreeMemory(p_Conn->psendMemPointer);  //释放内存
                    p_Conn->psendMemPointer = NULL;
                    p_Conn->iThrowsendCount = 0;      
                    continue;
                }
                else if(sendsize == -1)
                {
                    //发送缓冲区已经满了
                    ++p_Conn->iThrowsendCount; //标记发送缓冲区满了，需要通过epoll事件来驱动消息的继续发送
                    //投递此事件后，我们将依靠epoll驱动调用mkd_write_request_handler()函数发送数据
                    if(pSocketObj->mkd_epoll_oper_event(
                                p_Conn->fd,         //socket句柄
                                EPOLL_CTL_MOD,      //事件类型，这里是增加【因为我们准备增加个写通知】
                                EPOLLOUT,           //标志，这里代表要增加的标志,EPOLLOUT：可写【可写的时候通知我】
                                0,                  //对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数, 0：增加   1：去掉 2：完全覆盖
                                p_Conn              //连接池中的连接
                                ) == -1)
                    {
                        mkd_log_stderr(errno,"CSocekt::ServerSendQueueThread()中mkd_epoll_add_event()_2失败.");
                    }
                    continue;
                }
                else
                {
                    //等待recv()来做断开socket以及回收资源
                    p_memory->FreeMemory(p_Conn->psendMemPointer);  //释放内存
                    p_Conn->psendMemPointer = NULL;
                    p_Conn->iThrowsendCount = 0;  
                    continue;
                }

            } 
            err = pthread_mutex_unlock(&pSocketObj->m_sendMessageQueueMutex); 
            if(err != 0)  mkd_log_stderr(err,"CSocekt::ServerSendQueueThread()pthread_mutex_unlock()失败，返回的错误码为%d!",err);
            
        } 
    } 
    
    return (void*)0;
}
