
#定义项目编译的根目录,通过export把某个变量声明为全局的[其他文件中可以用]
export BUILD_ROOT = $(shell pwd)

#定义头文件的路径变量
export INCLUDE_PATH = $(BUILD_ROOT)/_include

#定义要编译的目录
BUILD_DIR = $(BUILD_ROOT)/signal/ \
			$(BUILD_ROOT)/proc/   \
			$(BUILD_ROOT)/net/    \
			$(BUILD_ROOT)/misc/   \
			$(BUILD_ROOT)/logic/   \
			$(BUILD_ROOT)/app/ 

#很多调试工具，包括Valgrind工具集都会因为这个为true能够输出更多的调试信息
export DEBUG = true

