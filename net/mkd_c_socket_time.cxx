
//网络中与时间相关
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

//设置踢出时钟(向multimap表中增加内容)，用户三次握手成功连入，然后开启踢人开关【Sock_WaitTimeEnable = 1】，那么本函数被调用；
void CSocekt::AddToTimerQueue(lpmkd_connection_t pConn)
{
    CMemory *p_memory = CMemory::GetInstance();

    time_t futtime = time(NULL);
    futtime += m_iWaitTime;  //20秒之后的时间

    CLock lock(&m_timequeueMutex); //互斥，因为要操作m_timeQueuemap
    LPSTRUC_MSG_HEADER tmpMsgHeader = (LPSTRUC_MSG_HEADER)p_memory->AllocMemory(m_iLenMsgHeader,false);
    tmpMsgHeader->pConn = pConn;
    tmpMsgHeader->iCurrsequence = pConn->iCurrsequence;
    m_timerQueuemap.insert(std::make_pair(futtime,tmpMsgHeader)); //按键 自动排序 小->大
    m_cur_size_++;  //计时队列尺寸+1
    m_timer_value_ = GetEarliestTime(); //计时队列头部时间值保存到m_timer_value_里
    return;    
}

//从multimap中取得最早的时间返回去，调用者负责互斥
time_t CSocekt::GetEarliestTime()
{
    std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos;	
	pos = m_timerQueuemap.begin();		
	return pos->first;	
}

//从m_timeQueuemap移除最早的时间，并把最早这个时间所在的项的值所对应的指针返回，调用者负责互斥
LPSTRUC_MSG_HEADER CSocekt::RemoveFirstTimer()
{
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos;	
	LPSTRUC_MSG_HEADER p_tmp;
	if(m_cur_size_ <= 0)
	{
		return NULL;
	}
	pos = m_timerQueuemap.begin(); //调用者负责互斥的，这里直接操作没问题的
	p_tmp = pos->second;
	m_timerQueuemap.erase(pos);
	--m_cur_size_;
	return p_tmp;
}

//根据给的当前时间，从m_timeQueuemap找到比这个时间更老（更早）的节点【1个】返回去，这些节点都是时间超过了，要处理的节点
//调用者负责互斥，所以本函数不用互斥
LPSTRUC_MSG_HEADER CSocekt::GetOverTimeTimer(time_t cur_time)
{	
	CMemory *p_memory = CMemory::GetInstance();
	LPSTRUC_MSG_HEADER ptmp;

	if (m_cur_size_ == 0 || m_timerQueuemap.empty())
		return NULL; //队列为空

	time_t earliesttime = GetEarliestTime(); //到multimap中去查询
	if (earliesttime <= cur_time)
	{
		//存在超时的节点
		ptmp = RemoveFirstTimer();    //把这个超时的节点从m_timerQueuemap 删掉，并把这个节点的第二项返回来；

        //因为下次超时的时间也依然要判断，所以还要把这个节点加回来        
		time_t newinqueutime = cur_time+(m_iWaitTime);
		LPSTRUC_MSG_HEADER tmpMsgHeader = (LPSTRUC_MSG_HEADER)p_memory->AllocMemory(sizeof(STRUC_MSG_HEADER),false);
		tmpMsgHeader->pConn = ptmp->pConn;
		tmpMsgHeader->iCurrsequence = ptmp->iCurrsequence;			
		m_timerQueuemap.insert(std::make_pair(newinqueutime,tmpMsgHeader)); //自动排序 小->大			
		m_cur_size_++;       

		if(m_cur_size_ > 0) 
		{
			m_timer_value_ = GetEarliestTime(); //计时队列头部时间值保存到m_timer_value_里
		}
		return ptmp;
	}
	return NULL;
}

//把指定用户tcp连接从timer表中删出
void CSocekt::DeleteFromTimerQueue(lpmkd_connection_t pConn)
{
    std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos,posend;
	CMemory *p_memory = CMemory::GetInstance();

    CLock lock(&m_timequeueMutex);
lblMTQM:
	pos    = m_timerQueuemap.begin();
	posend = m_timerQueuemap.end();
	for(; pos != posend; ++pos)	
	{
		if(pos->second->pConn == pConn)
		{			
			p_memory->FreeMemory(pos->second);  //释放内存
			m_timerQueuemap.erase(pos);
			--m_cur_size_; //减去一个元素，必然要把尺寸减少1个;								
			goto lblMTQM;
		}		
	}
	if(m_cur_size_ > 0)
	{
		m_timer_value_ = GetEarliestTime();
	}
    return;    
}

//清理时间队列中所有内容
void CSocekt::clearAllFromTimerQueue()
{	
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos,posend;

	CMemory *p_memory = CMemory::GetInstance();	
	pos    = m_timerQueuemap.begin();
	posend = m_timerQueuemap.end();    
	for(; pos != posend; ++pos)	
	{
		p_memory->FreeMemory(pos->second);		
		--m_cur_size_; 		
	}
	m_timerQueuemap.clear();
}

//时间队列监视和处理线程，处理到期不发心跳包的用户踢出的线程
void* CSocekt::ServerTimerQueueMonitorThread(void* threadData)
{
    ThreadItem *pThread = static_cast<ThreadItem*>(threadData);
    CSocekt *pSocketObj = pThread->_pThis;

    time_t absolute_time,cur_time;
    int err;

    while(g_stopEvent == 0) //不退出
    {		
		if(pSocketObj->m_cur_size_ > 0)//队列不为空，有内容
        {
			//时间队列中最近发生事情的时间放到 absolute_time里
            absolute_time = pSocketObj->m_timer_value_; 
            cur_time = time(NULL);
            if(absolute_time < cur_time)
            {
                //时间到了，可以处理
                std::list<LPSTRUC_MSG_HEADER> m_lsIdleList; //保存要处理的内容
                LPSTRUC_MSG_HEADER result;

                err = pthread_mutex_lock(&pSocketObj->m_timequeueMutex);  
                if(err != 0) mkd_log_stderr(err,"CSocekt::ServerTimerQueueMonitorThread()中pthread_mutex_lock()失败，返回的错误码为%d!",err);//有问题，要及时报告
                while ((result = pSocketObj->GetOverTimeTimer(cur_time)) != NULL) //一次性的把所有超时节点都拿过来
				{
					m_lsIdleList.push_back(result); 
				}
                err = pthread_mutex_unlock(&pSocketObj->m_timequeueMutex); 
                if(err != 0)  mkd_log_stderr(err,"CSocekt::ServerTimerQueueMonitorThread()pthread_mutex_unlock()失败，返回的错误码为%d!",err);//有问题，要及时报告                
                LPSTRUC_MSG_HEADER tmpmsg;
                while(!m_lsIdleList.empty())
                {
                    tmpmsg = m_lsIdleList.front();
					m_lsIdleList.pop_front(); 
                    pSocketObj->procPingTimeOutChecking(tmpmsg,cur_time); //这里需要检查心跳超时问题
                } 
            }
        }
        
        usleep(500 * 1000); 
    } //end while

    return (void*)0;
}

//心跳包检测时间到，该去检测心跳包是否超时的事宜，本函数只是把内存释放，子类应该重新事先该函数以实现具体的判断动作
void CSocekt::procPingTimeOutChecking(LPSTRUC_MSG_HEADER tmpmsg,time_t cur_time)
{
	CMemory *p_memory = CMemory::GetInstance();
	p_memory->FreeMemory(tmpmsg);    
}


