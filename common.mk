
#.PHONY:all clean 

ifeq ($(DEBUG),true)
#-g是生成调试信息。GNU调试器可以利用该信息
CC = g++ -std=c++11 -g 
VERSION = debug
else
CC = g++ -std=c++11
VERSION = release
endif

# $(wildcard *.c)表示扫描当前目录下所有.c文件
SRCS = $(wildcard *.cxx)

OBJS = $(SRCS:.cxx=.o)

#把字符串中的.c替换为.d
DEPS = $(SRCS:.cxx=.d)

#指定BIN文件的位置,addprefix是增加前缀函数
BIN := $(addprefix $(BUILD_ROOT)/,$(BIN))

#定义存放ojb文件的目录
LINK_OBJ_DIR = $(BUILD_ROOT)/app/link_obj
DEP_DIR = $(BUILD_ROOT)/app/dep

$(shell mkdir -p $(LINK_OBJ_DIR))
$(shell mkdir -p $(DEP_DIR))

#把目标文件生成到上述目标文件目录去，利用函数addprefix增加前缀
OBJS := $(addprefix $(LINK_OBJ_DIR)/,$(OBJS))
DEPS := $(addprefix $(DEP_DIR)/,$(DEPS))

#找到目录中的所有.o文件
LINK_OBJ = $(wildcard $(LINK_OBJ_DIR)/*.o)
LINK_OBJ += $(OBJS)

#make找第一个目标开始执行
#开始执行的入口
all:$(DEPS) $(OBJS) $(BIN)

#这里是诸多.d文件被包含进来，每个.d文件里都记录着一个.o文件所依赖的.c和.h文件。内容诸如 mkdyd.o: mkdyd.c mkd_func.h
ifneq ("$(wildcard $(DEPS))","")   
include $(DEPS)  
endif

#$(BIN):$(OBJS)
$(BIN):$(LINK_OBJ)
	@echo "------------------------build $(VERSION) mode--------------------------------!!!"

#$@：目标     $^：所有目标依赖
# gcc -o 是生成可执行文件
	$(CC) -o $@ $^ -lpthread

#%.o:%.c
$(LINK_OBJ_DIR)/%.o:%.cxx
# gcc -c是生成.o目标文件   -I可以指定头文件的路径
#$(CC) -o $@ -c $^
	$(CC) -I$(INCLUDE_PATH) -o $@ -c $(filter %.cxx,$^)

#当修改一个.h时，能够让make自动重新编译，所以需要指明让.o依赖于.h文件
#一个.o依赖于的.h文件，可以用“gcc -MM c程序文件名” 来获得这些依赖信息并重定向保存到.d文件中
#.d文件中的内容形如：mkdyd.o: mkdyd.c mkd_func.h
#%.d:%.c
$(DEP_DIR)/%.d:%.cxx
#gcc -MM $^ > $@
#echo 中 -n表示后续追加不换行
	echo -n $(LINK_OBJ_DIR)/ > $@
#  >>表示追加
#	gcc -I$(INCLUDE_PATH) -MM $^ >> $@
	$(CC) -I$(INCLUDE_PATH) -MM $^ >> $@





