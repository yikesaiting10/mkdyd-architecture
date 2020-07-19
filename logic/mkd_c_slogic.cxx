
//网络以及逻辑处理相关

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
#include "mkd_c_memory.h"
#include "mkd_c_crc32.h"
#include "mkd_c_slogic.h"  
#include "mkd_logiccomm.h"  
#include "mkd_c_lockmutex.h"  

//定义成员函数指针
typedef bool (CLogicSocket::*handler)(  lpmkd_connection_t pConn,      //连接池中连接的指针
                                        LPSTRUC_MSG_HEADER pMsgHeader,  //消息头指针
                                        char *pPkgBody,                 //包体指针
                                        unsigned short iBodyLength);    //包体长度

//用来保存成员函数指针数组
static const handler statusHandler[] = 
{
    //数组前5个元素
    &CLogicSocket::_HandlePing,                             //【0】：心跳包的实现
    NULL,                                                   //【1】：下标从0开始
    NULL,                                                   //【2】：下标从0开始
    NULL,                                                   //【3】：下标从0开始
    NULL,                                                   //【4】：下标从0开始

    //开始处理具体的业务逻辑
    &CLogicSocket::_HandleRegister,                         //【5】：实现具体的注册功能
    &CLogicSocket::_HandleLogIn,                            //【6】：实现具体的登录功能

};
#define AUTH_TOTAL_COMMANDS sizeof(statusHandler)/sizeof(handler) 
//构造函数
CLogicSocket::CLogicSocket()
{

}
//析构函数
CLogicSocket::~CLogicSocket()
{

}

//初始化函数
bool CLogicSocket::Initialize()
{    
    bool bParentInit = CSocekt::Initialize();  //调用父类的同名函数
    return bParentInit;
}

//处理收到的数据包，由线程池来调用本函数，本函数是一个单独的线程；
//pMsgBuf：消息头 + 包头 + 包体
void CLogicSocket::threadRecvProcFunc(char *pMsgBuf)
{          
    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;                  //消息头
    LPCOMM_PKG_HEADER  pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf+m_iLenMsgHeader); //包头
    void  *pPkgBody;                                                              //指向包体的指针
    unsigned short pkglen = ntohs(pPkgHeader->pkgLen);                            //客户端指明的包宽度【包头+包体】

    if(m_iLenPkgHeader == pkglen)
    {
        //没有包体，只有包头
		if(pPkgHeader->crc32 != 0) //只有包头的crc值给0
		{
			return; //crc错，直接丢弃
		}
		pPkgBody = NULL;
    }
    else 
	{
        //有包体，走到这里
		pPkgHeader->crc32 = ntohl(pPkgHeader->crc32);		          //针对4字节的数据，网络序转主机序
		pPkgBody = (void *)(pMsgBuf+m_iLenMsgHeader+m_iLenPkgHeader); //跳过消息头 以及 包头 ，指向包体

		//计算crc值判断包的完整性        
		int calccrc = CCRC32::GetInstance()->Get_CRC((unsigned char *)pPkgBody,pkglen-m_iLenPkgHeader); //计算纯包体的crc值
		if(calccrc != pPkgHeader->crc32) //服务器端根据包体计算crc值，和客户端传递过来的包头中的crc32信息比较
		{
            mkd_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中CRC错误，丢弃数据!");   
			return; //crc错，直接丢弃
		}
	}

    //包crc校验OK才能走到这里    	
    unsigned short imsgCode = ntohs(pPkgHeader->msgCode); //消息代码拿出来
    lpmkd_connection_t p_Conn = pMsgHeader->pConn;        //消息头中藏着连接池中连接的指针

    //如果从收到客户端发送来的包，到服务器释放一个线程池中的线程处理该包的过程中，客户端断开了，这种收到的包就不必处理了  
    if(p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)   //该连接池中连接以被其他tcp连接【其他socket】占用，这说明原来的客户端和本服务器的连接断了，这种包直接丢弃不理
    {
        return; 
    }

    //判断消息码是正确的，防止客户端恶意侵害服务器，发送一个不在服务器处理范围内的消息码
	if(imsgCode >= AUTH_TOTAL_COMMANDS) //无符号数不可能<0
    {
        mkd_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码不对!",imsgCode); 
        return; 
    }

    //有对应的消息处理函数码
    if(statusHandler[imsgCode] == NULL) 
    {
        mkd_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码找不到对应的处理函数!",imsgCode); 
        return;  
    }

    //调用消息码对应的成员函数来处理
    (this->*statusHandler[imsgCode])(p_Conn,pMsgHeader,(char *)pPkgBody,pkglen-m_iLenPkgHeader);
    return;	
}

//心跳包检测时间到，该去检测心跳包是否超时的事宜，本函数是子类函数，实现具体的判断动作
void CLogicSocket::procPingTimeOutChecking(LPSTRUC_MSG_HEADER tmpmsg,time_t cur_time)
{
    CMemory *p_memory = CMemory::GetInstance();

    if(tmpmsg->iCurrsequence == tmpmsg->pConn->iCurrsequence) //此连接没断
    {
        lpmkd_connection_t p_Conn = tmpmsg->pConn;

        //超时踢的判断标准就是 每次检查的时间间隔*3，超过这个时间没发送心跳包就踢掉
        if( (cur_time - p_Conn->lastPingTime ) > (m_iWaitTime*3+10) )
        {
            mkd_log_stderr(0,"时间到不发心跳包，踢出去!");   
            zdClosesocketProc(p_Conn); 
        }   
             
        p_memory->FreeMemory(tmpmsg);//内存要释放
    }
    else //此连接断了
    {
        p_memory->FreeMemory(tmpmsg);//内存要释放
    }
    return;
}

//发送没有包体的数据包给客户端
void CLogicSocket::SendNoBodyPkgToClient(LPSTRUC_MSG_HEADER pMsgHeader,unsigned short iMsgCode)
{
    CMemory  *p_memory = CMemory::GetInstance();

    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader+m_iLenPkgHeader,false);
    char *p_tmpbuf = p_sendbuf;
    
	memcpy(p_tmpbuf,pMsgHeader,m_iLenMsgHeader);
	p_tmpbuf += m_iLenMsgHeader;

    LPCOMM_PKG_HEADER pPkgHeader = (LPCOMM_PKG_HEADER)p_tmpbuf;	  //指向的是要发送出去的包的包头	
    pPkgHeader->msgCode = htons(iMsgCode);	
    pPkgHeader->pkgLen = htons(m_iLenPkgHeader); 
	pPkgHeader->crc32 = 0;		
    msgSend(p_sendbuf);
    return;
}

//----------------------------------------------------------------------------------------------------------
//处理各种业务逻辑
bool CLogicSocket::_HandleRegister(lpmkd_connection_t pConn,LPSTRUC_MSG_HEADER pMsgHeader,char *pPkgBody,unsigned short iBodyLength)
{
    mkd_log_stderr(0,"执行了CLogicSocket::_HandleRegister()!");
    
    //首先判断包体的合法性
    if(pPkgBody == NULL) 
    {        
        return false;
    }
		    
    int iRecvLen = sizeof(STRUCT_REGISTER); 
    if(iRecvLen != iBodyLength) 
    {     
        return false; 
    }

    CLock lock(&pConn->logicPorcMutex); //凡是和本用户有关的访问都互斥
    
    //取得了整个发送过来的数据
    LPSTRUCT_REGISTER p_RecvInfo = (LPSTRUCT_REGISTER)pPkgBody; 

    //给客户端返回数据时，一般也是返回一个结构
	LPCOMM_PKG_HEADER pPkgHeader;	
	CMemory  *p_memory = CMemory::GetInstance();
	CCRC32   *p_crc32 = CCRC32::GetInstance();
    int iSendLen = sizeof(STRUCT_REGISTER);  
   
    //分配要发送出去的包的内存
    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader+m_iLenPkgHeader+iSendLen,false);//准备发送的格式：消息头+包头+包体
    //填充消息头
    memcpy(p_sendbuf,pMsgHeader,m_iLenMsgHeader);                   //消息头直接拷贝到这里来
    //填充包头
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf+m_iLenMsgHeader);    //指向包头
    pPkgHeader->msgCode = _CMD_REGISTER;	                        //消息代码
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode);	            //htons主机序转网络序 
    pPkgHeader->pkgLen  = htons(m_iLenPkgHeader + iSendLen);        //整个包的尺寸【包头+包体尺寸】 
    //填充包体
    LPSTRUCT_REGISTER p_sendInfo = (LPSTRUCT_REGISTER)(p_sendbuf+m_iLenMsgHeader+m_iLenPkgHeader);	//跳过消息头，跳过包头，就是包体了
    
    //包体内容全部确定好后，计算包体的crc32值
    pPkgHeader->crc32   = p_crc32->Get_CRC((unsigned char *)p_sendInfo,iSendLen);
    pPkgHeader->crc32   = htonl(pPkgHeader->crc32);		

    //发送数据包
    msgSend(p_sendbuf);
    return true;
}
bool CLogicSocket::_HandleLogIn(lpmkd_connection_t pConn,LPSTRUC_MSG_HEADER pMsgHeader,char *pPkgBody,unsigned short iBodyLength)
{
    mkd_log_stderr(0,"执行了CLogicSocket::_HandleLogIn()!");
    return true;
}


//接收并处理客户端发送过来的ping包
bool CLogicSocket::_HandlePing(lpmkd_connection_t pConn,LPSTRUC_MSG_HEADER pMsgHeader,char *pPkgBody,unsigned short iBodyLength)
{
    //心跳包要求没有包体；
    if(iBodyLength != 0)  //有包体则认为是 非法包
		return false; 

    CLock lock(&pConn->logicPorcMutex); //凡是和本用户有关的访问都考虑用互斥，以免该用户同时发送过来两个命令达到各种作弊目的
    pConn->lastPingTime = time(NULL);   //更新该变量

    //服务器也发送 一个只有包头的数据包给客户端，作为返回的数据
    SendNoBodyPkgToClient(pMsgHeader,_CMD_PING);

    mkd_log_stderr(0,"成功收到了心跳包并返回结果！");
    return true;
}