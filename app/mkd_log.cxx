//和日志相关的函数
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

#include "mkd_global.h"
#include "mkd_macro.h"
#include "mkd_func.h"
#include "mkd_c_conf.h"

//全局量---------------------
//错误等级，和mkd_macro.h里定义的日志等级宏是一一对应关系
static u_char err_levels[][20]  = 
{
    {"stderr"},    //0：控制台错误
    {"emerg"},     //1：紧急
    {"alert"},     //2：警戒
    {"crit"},      //3：严重
    {"error"},     //4：错误
    {"warn"},      //5：警告
    {"notice"},    //6：注意
    {"info"},      //7：信息
    {"debug"}      //8：调试
};
mkd_log_t   mkd_log;

void mkd_log_stderr(int err, const char *fmt, ...)
{    
    va_list args;                        //创建一个va_list类型变量
    u_char  errstr[MKD_MAX_ERROR_STR+1]; 
    u_char  *p,*last;

    memset(errstr,0,sizeof(errstr));     

    last = errstr + MKD_MAX_ERROR_STR;        //last指向整个buffer最后一个有效位置的后面也就是非有效位
                                                
    p = mkd_cpymem(errstr, "mkdyd: ", 7);     //p指向"mkdyd: "之后    
    
    va_start(args, fmt); //使args指向起始的参数
    p = mkd_vslprintf(p, last, fmt, args); //组合出这个字符串保存在errstr里
    va_end(args);        //释放args

    if (err)  //如果错误代码不是0，表示有错误发生
    {
        //错误代码和错误信息也要显示出来
        p = mkd_log_errno(p, last, err);
    }
     
    if (p >= (last - 1))
    {
        p = (last - 1) - 1; //把尾部空格留出来 
    }
    *p++ = '\n'; //增加个换行符    
    
    //往标准错误【一般是屏幕】输出信息    
    write(STDERR_FILENO,errstr,p - errstr); 

    if(mkd_log.fd > STDERR_FILENO) 
    {
        err = 0;    
        p--;*p = 0;  
        mkd_log_error_core(MKD_LOG_STDERR,err,(const char *)errstr); 
    }    
    return;
}

//描述：给一段内存，一个错误编号，组合出一个字符串，形如：(错误编号: 错误原因)，放到给的这段内存中去
//buf：是个内存，要往这里保存数据
//last：放的数据不要超过这里
//err：错误编号，取得这个错误编号对应的错误字符串，保存到buffer中
u_char *mkd_log_errno(u_char *buf, u_char *last, int err)
{
    char *perrorinfo = strerror(err); 
    size_t len = strlen(perrorinfo);

    char leftstr[10] = {0}; 
    sprintf(leftstr," (%d: ",err);
    size_t leftlen = strlen(leftstr);

    char rightstr[] = ") "; 
    size_t rightlen = strlen(rightstr);
    
    size_t extralen = leftlen + rightlen; //左右的额外宽度
    if ((buf + len + extralen) < last)
    {
        buf = mkd_cpymem(buf, leftstr, leftlen);
        buf = mkd_cpymem(buf, perrorinfo, len);
        buf = mkd_cpymem(buf, rightstr, rightlen);
    }
    return buf;
}
 
//往日志文件中写日志
//level:一个等级数字，我们把日志分成一些等级，以方便管理、显示、过滤等等，如果这个等级数字比配置文件中的等级数字"LogLevel"大，那么该条信息不被写到日志文件中
//err：是个错误代码，如果不是0，就应该转换成显示对应的错误信息,一起写到日志文件中，
//mkd_log_error_core(5,8,"这个XXX工作的有问题,显示的结果是=%s","YYYY");
void mkd_log_error_core(int level,  int err, const char *fmt, ...)
{
    u_char  *last;
    u_char  errstr[MKD_MAX_ERROR_STR+1];   
    memset(errstr,0,sizeof(errstr));  
    last = errstr + MKD_MAX_ERROR_STR;   
    
    struct timeval   tv;
    struct tm        tm;
    time_t           sec;   
    u_char           *p;    //指向当前要拷贝数据到其中的内存位置
    va_list          args;

    memset(&tv,0,sizeof(struct timeval));    
    memset(&tm,0,sizeof(struct tm));

    gettimeofday(&tv, NULL);     //获取当前时间

    sec = tv.tv_sec;             //秒
    localtime_r(&sec, &tm);      //把参数1的time_t转换为本地时间，保存到参数2中
    tm.tm_mon++;                 //月份调整
    tm.tm_year += 1900;          //年份调整
    
    u_char strcurrtime[40]={0};  //先组合出一个当前时间字符串，格式形如：2020/06/08 19:57:11
    mkd_slprintf(strcurrtime,  
                    (u_char *)-1,                       
                    "%4d/%02d/%02d %02d:%02d:%02d",     
                    tm.tm_year, tm.tm_mon,
                    tm.tm_mday, tm.tm_hour,
                    tm.tm_min, tm.tm_sec);
    p = mkd_cpymem(errstr,strcurrtime,strlen((const char *)strcurrtime));  
    p = mkd_slprintf(p, last, " [%s] ", err_levels[level]);                
    p = mkd_slprintf(p, last, "%P: ",mkd_pid);                             

    va_start(args, fmt);                     //使args指向起始的参数
    p = mkd_vslprintf(p, last, fmt, args);   //把fmt和args参数弄进去，组合出来这个字符串
    va_end(args);                            //释放args 

    if (err)  //如果错误代码不是0，表示有错误发生
    {
        //错误代码和错误信息也要显示出来
        p = mkd_log_errno(p, last, err);
    }
    //若位置不够，那换行插入到末尾
    if (p >= (last - 1))
    {
        p = (last - 1) - 1; 
                             
    }
    *p++ = '\n';    

    ssize_t   n;
    while(1) 
    {        
        if (level > mkd_log.log_level) 
        {
            //要打印的日志的等级太落后，不打印
            break;
        }

        //写日志文件        
        n = write(mkd_log.fd, errstr, p - errstr);  
        if (n == -1) 
        {
            //写失败有问题
            if(errno == ENOSPC) //写失败，且原因是磁盘没空间了
            {
                //先do something；
            }
            else
            {
                //其他错误，把这个错误显示到标准错误设备
                if(mkd_log.fd != STDERR_FILENO) //当前是定位到文件的，则条件成立
                {
                    n = write(STDERR_FILENO,errstr,p - errstr);
                }
            }
        }
        break;
    }    
    return;
}

//日志初始化
void mkd_log_init()
{
    u_char *plogname = NULL;
    size_t nlen;

    //从配置文件中读取和日志相关的配置信息
    CConfig *p_config = CConfig::GetInstance();
    plogname = (u_char *)p_config->GetString("Log");
    if(plogname == NULL)
    {
        //没读到，给缺省的路径文件名
        plogname = (u_char *) MKD_ERROR_LOG_PATH; 
    }
    mkd_log.log_level = p_config->GetIntDefault("LogLevel",MKD_LOG_NOTICE);

    mkd_log.fd = open((const char *)plogname,O_WRONLY|O_APPEND|O_CREAT,0644);  
    if (mkd_log.fd == -1)  //如果有错误，则直接定位到标准错误
    {
        mkd_log_stderr(errno,"[alert] could not open error log file: open() \"%s\" failed", plogname);
        mkd_log.fd = STDERR_FILENO;         
    } 
    return;
}
