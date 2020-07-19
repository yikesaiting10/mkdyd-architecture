﻿
#ifndef __MKD_COMM_H__
#define __MKD_COMM_H__

//宏定义------------------------------------
#define _PKG_MAX_LENGTH     30000  //每个包的最大长度【包头+包体】

//通信 收包状态定义
#define _PKG_HD_INIT         0  //初始状态，准备接收数据包头
#define _PKG_HD_RECVING      1  //接收包头中，包头不完整，继续接收中
#define _PKG_BD_INIT         2  //包头刚好收完，准备接收包体
#define _PKG_BD_RECVING      3  //接收包体中，包体不完整，继续接收中，处理后直接回到_PKG_HD_INIT状态

#define _DATA_BUFSIZE_       20  //因为我要先收包头，我希望定义一个固定大小的数组专门用来收包头，这个数字大小一定要 >sizeof(COMM_PKG_HEADER) 

//结构定义------------------------------------
#pragma pack (1) //对齐方式,1字节对齐【结构之间成员不做任何字节对齐：紧密的排列在一起】

//网络通讯相关的结构
typedef struct _COMM_PKG_HEADER
{
	unsigned short pkgLen;    //报文总长度【包头+包体】--2字节
	                            //包头中记录着整个包【包头+包体】的长度

	unsigned short msgCode;   //消息类型代码--2字节，用于区别每个不同的命令【不同的消息】
	int            crc32;     //CRC32效验--4字节，为了防止收发数据中出现收到内容和发送内容不一致的情况，引入这个字段做校验
}COMM_PKG_HEADER,*LPCOMM_PKG_HEADER;


#pragma pack() //取消指定对齐，恢复缺省对齐


#endif
