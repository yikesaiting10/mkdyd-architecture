//打印格式相关
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>   

#include "mkd_global.h"
#include "mkd_macro.h"
#include "mkd_func.h"

static u_char *mkd_sprintf_num(u_char *buf, u_char *last, uint64_t ui64,u_char zero, uintptr_t hexadecimal, uintptr_t width);

//----------------------------------------------------------------------------------------------------------------------
//该函数相当于针对mkd_vslprintf()函数包装了一下
u_char *mkd_slprintf(u_char *buf, u_char *last, const char *fmt, ...) 
{
    va_list   args;
    u_char   *p;

    va_start(args, fmt); //使args指向起始的参数
    p = mkd_vslprintf(buf, last, fmt, args);
    va_end(args);        //释放args   
    return p;
}

//和上边的mkd_snprintf类似
u_char * mkd_snprintf(u_char *buf, size_t max, const char *fmt, ...)   //类printf()格式化函数，比较安全，max指明了缓冲区结束位置
{
    u_char   *p;
    va_list   args;

    va_start(args, fmt);
    p = mkd_vslprintf(buf, buf + max, fmt, args);
    va_end(args);
    return p;
}

//buf：往这里放数据
//last：放的数据不要超过这里
//fmt：以这个为首的一系列可变参数
u_char *mkd_vslprintf(u_char *buf, u_char *last,const char *fmt,va_list args)
{   
    u_char     zero;

    uintptr_t  width,sign,hex,frac_width,scale,n;  //临时用到的一些变量

    int64_t    i64;   
    uint64_t   ui64;  
    u_char     *p;    
    double     f;     
    uint64_t   frac;  
    

    while (*fmt && buf < last) 
    {
        if (*fmt == '%')  //%开头的一般都是需要被可变参数取代 
        {
            //-----------------变量初始化工作开始-----------------
            zero  = (u_char) ((*++fmt == '0') ? '0' : ' ');  
                                                                
            width = 0;                                       
            sign  = 1;                                       
            hex   = 0;                                       
            frac_width = 0;                                  
            i64 = 0;                                         
            ui64 = 0;                                           
            
            //-----------------变量初始化工作结束-----------------

            while (*fmt >= '0' && *fmt <= '9')     
            {
                width = width * 10 + (*fmt++ - '0');
            }

            for ( ;; ) 
            {
                switch (*fmt)  
                {
                case 'u':       
                    sign = 0;   
                    fmt++;      
                    continue;   

                case 'X':       
                    hex = 2;    
                    sign = 0;
                    fmt++;
                    continue;
                case 'x':       
                    hex = 1;    
                    sign = 0;
                    fmt++;
                    continue;

                case '.':       
                    fmt++;      
                    while(*fmt >= '0' && *fmt <= '9')  
                    {
                        frac_width = frac_width * 10 + (*fmt++ - '0'); 
                    }  
                    break;

                default:
                    break;                
                } 
                break;
            } 

            switch (*fmt) 
            {
            case '%': 
                *buf++ = '%';
                fmt++;
                continue;
        
            case 'd': 
                if (sign)  
                {
                    i64 = (int64_t) va_arg(args, int);  
                }
                else 
                {
                    ui64 = (uint64_t) va_arg(args, u_int);    
                }
                break;  

             case 'i': 
                if (sign) 
                {
                    i64 = (int64_t) va_arg(args, intptr_t);
                } 
                else 
                {
                    ui64 = (uint64_t) va_arg(args, uintptr_t);
                }

                break;    

            case 'L':  
                if (sign)
                {
                    i64 = va_arg(args, int64_t);
                } 
                else 
                {
                    ui64 = va_arg(args, uint64_t);
                }
                break;

            case 'p':  
                ui64 = (uintptr_t) va_arg(args, void *); 
                hex = 2;    
                sign = 0;   
                zero = '0'; 
                width = 2 * sizeof(void *);
                break;

            case 's': 
                p = va_arg(args, u_char *); 

                while (*p && buf < last)  
                {
                    *buf++ = *p++;  
                }
                
                fmt++;
                continue; 

            case 'P':  
                i64 = (int64_t) va_arg(args, pid_t);
                sign = 1;
                break;

            case 'f': 
                f = va_arg(args, double);  
                if (f < 0)  
                {
                    *buf++ = '-'; 
                    f = -f; 
                }
                
                ui64 = (int64_t) f; 
                frac = 0;

                if (frac_width) 
                {
                    scale = 1;  
                    for (n = frac_width; n; n--) 
                    {
                        scale *= 10; 
                    }
                    frac = (uint64_t) ((f - (double) ui64) * scale + 0.5);   
                    if (frac == scale)   
                    {
                        ui64++;    
                        frac = 0;  
                    }
                }

                buf = mkd_sprintf_num(buf, last, ui64, zero, 0, width); 

                if (frac_width) 
                {
                    if (buf < last) 
                    {
                        *buf++ = '.'; 
                    }
                    buf = mkd_sprintf_num(buf, last, frac, '0', 0, frac_width); 
                }
                fmt++;
                continue;  

            default:
                *buf++ = *fmt++; 
                continue; 
            } 
            
            if (sign) 
            {
                if (i64 < 0)  
                {
                    *buf++ = '-';  
                    ui64 = (uint64_t) -i64; 
                }
                else 
                {
                    ui64 = (uint64_t) i64;
                }
            }  

            buf = mkd_sprintf_num(buf, last, ui64, zero, hex, width); 
            fmt++;
        }
        else  
        {
            *buf++ = *fmt++;   
        } 
    } 
    
    return buf;
}

static u_char * mkd_sprintf_num(u_char *buf, u_char *last, uint64_t ui64, u_char zero, uintptr_t hexadecimal, uintptr_t width)
{
    u_char      *p, temp[MKD_INT64_LEN + 1];   
    size_t      len;
    uint32_t    ui32;

    static u_char   hex[] = "0123456789abcdef";  
    static u_char   HEX[] = "0123456789ABCDEF";  

    p = temp + MKD_INT64_LEN; 

    if (hexadecimal == 0)  
    {
        if (ui64 <= (uint64_t) MKD_MAX_UINT32_VALUE)  
        {
            ui32 = (uint32_t) ui64; 
            do  
            {
                *--p = (u_char) (ui32 % 10 + '0');  
            }
            while (ui32 /= 10); 
        }
        else
        {
            do 
            {
                *--p = (u_char) (ui64 % 10 + '0');
            } while (ui64 /= 10); 
        }
    }
    else if (hexadecimal == 1)  
    {
        do 
        {
            *--p = hex[(uint32_t) (ui64 & 0xf)];    
        } while (ui64 >>= 4);    
    } 
    else 
    { 
        do 
        { 
            *--p = HEX[(uint32_t) (ui64 & 0xf)];
        } while (ui64 >>= 4);
    }

    len = (temp + MKD_INT64_LEN) - p;  

    while (len++ < width && buf < last)  
    {
        *buf++ = zero;  
    }
    
    len = (temp + MKD_INT64_LEN) - p; 
    if((buf + len) >= last)   
    {
        len = last - buf; 
    }

    return mkd_cpymem(buf, p, len); 
}

