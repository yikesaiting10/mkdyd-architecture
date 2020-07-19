//函数声明

#ifndef __MKD_FUNC_H__
#define __MKD_FUNC_H__

//字符串相关函数
void   Rtrim(char *string);
void   Ltrim(char *string);

//设置可执行程序标题相关函数
void   mkd_init_setproctitle();
void   mkd_setproctitle(const char *title);

//和日志，打印输出有关
void   mkd_log_init();
void   mkd_log_stderr(int err, const char *fmt, ...);
void   mkd_log_error_core(int level,  int err, const char *fmt, ...);
u_char *mkd_log_errno(u_char *buf, u_char *last, int err);
u_char *mkd_snprintf(u_char *buf, size_t max, const char *fmt, ...);
u_char *mkd_slprintf(u_char *buf, u_char *last, const char *fmt, ...);
u_char *mkd_vslprintf(u_char *buf, u_char *last,const char *fmt,va_list args);

//和信号/主流程相关相关
int    mkd_init_signals();
void   mkd_master_process_cycle();
int    mkd_daemon();
void   mkd_process_events_and_timers();


#endif  