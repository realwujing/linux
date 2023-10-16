/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_INIT_H
#define _LINUX_INIT_H

#include <linux/compiler.h>
#include <linux/types.h>

/* 内置 __init 函数不需要与 retpoline 一起编译 */
#if defined(__noretpoline) && !defined(MODULE)
#define __noinitretpoline __noretpoline
#else
#define __noinitretpoline
#endif

/* 这些宏用于标记某些函数或已初始化的数据（不适用于未初始化的数据）
 * 为“初始化”函数。内核可以将其视为提示，表明该函数仅在初始化阶段使用，
 * 并在使用后释放已使用的内存资源
 *
 * 用法：
 * 对于函数：
 *
 * 应在函数名之前立即添加 __init，例如：
 *
 * static void __init initme(int x, int y)
 * {
 *    extern int z; z = x * y;
 * }
 *
 * 如果函数在某处有原型，也可以在原型的右括号和分号之间添加 __init：
 *
 * extern int initialize_foobar_device(int, int, int) __init;
 *
 * 对于已初始化的数据：
 * 应在变量名和等号之间插入 __initdata 或 __initconst，后跟值，例如：
 *
 * static int init_variable __initdata = 0;
 * static const char linux_logo[] __initconst = { 0x32, 0x36, ... };
 *
 * 不要忘记初始化不在文件作用域内的数据，也就是在函数内，否则 gcc 将数据放入 bss 部分而不是 init 部分。
 */

/* 这些适用于所有人（尽管并非所有架构都会在模块中丢弃它） */
#define __init		__section(.init.text) __cold  __latent_entropy __noinitretpoline
#define __initdata	__section(.init.data)
#define __initconst	__section(.init.rodata)
#define __exitdata	__section(.exit.data)
#define __exit_call	__used __section(.exitcall.exit)

/*
 * modpost 检查内核构建过程中的段不匹配。
 * 当代码或数据段存在从初始化段（代码或数据）到引用时就会发生段不匹配。
 * 大多数架构中，当早期初始化完成后，内核会丢弃初始化段，因此所有此类引用都可能是错误。
 * 对于退出段，也存在相同的问题。
 *
 * 以下标记用于以下情况：引用到 *init / *exit 段（代码或数据）是有效的，并将教导
 * modpost 不发出警告。预期的语义是，标记为 __ref* 的代码或数据可以引用初始化段的代码或数据，而不会产生警告
 * （当然，没有警告并不意味着代码是正确的，所以最好记录为什么需要 __ref，以及为什么它是可以的）。
 *
 * 这些标记遵循与 __init / __initdata 相同的语法规则。
 */
#define __ref            __section(.ref.text) noinline
#define __refdata        __section(.ref.data)
#define __refconst       __section(.ref.rodata)

#ifdef MODULE
#define __exitused
#else
#define __exitused  __used
#endif

#define __exit          __section(.exit.text) __exitused __cold notrace

/* 用于 MEMORY_HOTPLUG */
#define __meminit        __section(.meminit.text) __cold notrace \
						  __latent_entropy
#define __meminitdata    __section(.meminit.data)
#define __meminitconst   __section(.meminit.rodata)
#define __memexit        __section(.memexit.text) __exitused __cold notrace
#define __memexitdata    __section(.memexit.data)
#define __memexitconst   __section(.memexit.rodata)

/* 用于汇编例程 */
#define __HEAD		.section	".head.text","ax"
#define __INIT		.section	".init.text","ax"
#define __FINIT		.previous

#define __INITDATA	.section	".init.data","aw",%progbits
#define __INITRODATA	.section	".init.rodata","a",%progbits
#define __FINITDATA	.previous

#define __MEMINIT        .section	".meminit.text", "ax"
#define __MEMINITDATA    .section	".meminit.data", "aw"
#define __MEMINITRODATA  .section	".meminit.rodata", "a"

/* 引用正确时取消警告 */
#define __REF            .section       ".ref.text", "ax"
#define __REFDATA        .section       ".ref.data", "aw"
#define __REFCONST       .section       ".ref.rodata", "a"

#ifndef __ASSEMBLY__
/*
 * 用于初始化调用..
 */
typedef int (*initcall_t)(void);  // 定义初始化调用函数指针类型
typedef void (*exitcall_t)(void);  // 定义退出调用函数指针类型

#ifdef CONFIG_HAVE_ARCH_PREL32_RELOCATIONS
typedef int initcall_entry_t;  // 初始化调用条目类型，当具有特定宏时使用
static inline initcall_t initcall_from_entry(initcall_entry_t *entry)
{
	return offset_to_ptr(entry);  // 从初始化调用条目获取初始化调用函数指针
}
#else
typedef initcall_t initcall_entry_t;  // 初始化调用条目类型，当没有特定宏时使用
static inline initcall_t initcall_from_entry(initcall_entry_t *entry)
{
	return *entry;  // 直接获取初始化调用函数指针
}
#endif

extern initcall_entry_t __con_initcall_start[], __con_initcall_end[];  // 构造器初始化调用条目的开始和结束
extern initcall_entry_t __security_initcall_start[], __security_initcall_end[];  // 安全性初始化调用条目的开始和结束

/* 用于构造器调用 */
typedef void (*ctor_fn_t)(void);  // 构造器函数指针类型

/* 在 init/main.c 中定义 */
extern int do_one_initcall(initcall_t fn);  // 执行单个初始化调用
extern char __initdata boot_command_line[];  // 引导命令行数据
extern char *saved_command_line;  // 保存的命令行字符串
extern unsigned int reset_devices;  // 重置设备标志

/* 由 init/main.c 使用 */
void setup_arch(char **);  // 设置体系结构
void prepare_namespace(void);  // 准备命名空间
void __init load_default_modules(void);  // 加载默认模块
int __init init_rootfs(void);  // 初始化根文件系统

#if defined(CONFIG_STRICT_KERNEL_RWX) || defined(CONFIG_STRICT_MODULE_RWX)
extern bool rodata_enabled;  // 只读数据段是否已启用
#endif
#ifdef CONFIG_STRICT_KERNEL_RWX
void mark_rodata_ro(void);  // 标记只读数据段为只读
#endif

extern void (*late_time_init)(void);  // 晚期时间初始化函数指针

extern bool initcall_debug;  // 初始化调用调试标志

#endif  // MODULE

#ifndef __ASSEMBLY__

/*
 * 初始化调用现在按功能分组到不同的子段中。
 * 子段内的排序由链接顺序决定。
 * 为了向后兼容，initcall() 将调用放入设备初始化子段中。
 *
 * __define_initcall() 中的 `id' 参数用于使多个 initcalls 可以指向同一个处理程序，
 * 而不会导致重复符号构建错误。
 *
 * 初始化调用通过在运行时将指针放置在初始化调用段中来运行。
 * 链接器可以进行死代码/数据消除并将其完全删除，因此初始化调用段必须在链接器脚本中标记为 KEEP()。
 */

#ifdef CONFIG_HAVE_ARCH_PREL32_RELOCATIONS
#define ___define_initcall(fn, id, __sec)			\
	__ADDRESSABLE(fn)					\
	asm(".section	\"" #__sec ".init\", \"a\"	\n"	\
	"__initcall_" #fn #id ":			\n"	\
	    ".long	" #fn " - .			\n"	\
	    ".previous					\n");
#else
#define ___define_initcall(fn, id, __sec) \
	static initcall_t __initcall_##fn##id __used \
		__attribute__((__section__(#__sec ".init"))) = fn;
#endif

#define __define_initcall(fn, id) ___define_initcall(fn, id, .initcall##id)

/*
 * 早期初始化调用在初始化 SMP 之前运行。
 *
 * 仅适用于内建代码，不适用于模块。
 */
#define early_initcall(fn)		__define_initcall(fn, early)

/*
 * "pure" initcall 没有依赖于其他任何内容，纯粹初始化了静态初始化不可能完成的变量。
 *
 * 这仅适用于内建代码，不适用于模块。
 * 保持 main.c:initcall_level_names[] 同步。
 */
#define pure_initcall(fn)		__define_initcall(fn, 0)

#define core_initcall(fn)		__define_initcall(fn, 1)
#define core_initcall_sync(fn)		__define_initcall(fn, 1s)
#define postcore_initcall(fn)		__define_initcall(fn, 2)
#define postcore_initcall_sync(fn)	__define_initcall(fn, 2s)
#define arch_initcall(fn)		__define_initcall(fn, 3)
#define arch_initcall_sync(fn)		__define_initcall(fn, 3s)
#define subsys_initcall(fn)		__define_initcall(fn, 4)
#define subsys_initcall_sync(fn)	__define_initcall(fn, 4s)
#define fs_initcall(fn)			__define_initcall(fn, 5)
#define fs_initcall_sync(fn)		__define_initcall(fn, 5s)
#define rootfs_initcall(fn)		__define_initcall(fn, rootfs)
#define device_initcall(fn)		__define_initcall(fn, 6)
#define device_initcall_sync(fn)	__define_initcall(fn, 6s)
#define late_initcall(fn)		__define_initcall(fn, 7)
#define late_initcall_sync(fn)		__define_initcall(fn, 7s)

#define __initcall(fn) device_initcall(fn)

#define __exitcall(fn)						\
	static exitcall_t __exitcall_##fn __exit_call = fn

#define console_initcall(fn)	___define_initcall(fn,, .con_initcall)
#define security_initcall(fn)	___define_initcall(fn,, .security_initcall)

struct obs_kernel_param {
	const char *str;
	int (*setup_func)(char *);
	int early;
};

/*
 * 仅适用于真正核心代码。查看 moduleparam.h 以获取正常方式。
 *
 * 强制对齐以防止编译器在 .init.setup 中过多地分隔 obs_kernel_param "array" 的元素。
 */
#define __setup_param(str, unique_id, fn, early)			\
	static const char __setup_str_##unique_id[] __initconst		\
		__aligned(1) = str; 					\
	static struct obs_kernel_param __setup_##unique_id		\
		__used __section(.init.setup)				\
		__attribute__((aligned((sizeof(long)))))		\
		= { __setup_str_##unique_id, fn, early }

#define __setup(str, fn)						\
	__setup_param(str, fn, fn, 0)

/*
*注意：fn 是根据 module_param，而不是 __setup！
*如果 fn 返回非零，则发出警告。
*/
#define early_param(str, fn)						\
	__setup_param(str, fn, fn, 1)

#define early_param_on_off(str_on, str_off, var, config)		\
									\
	int var = IS_ENABLED(config);					\
									\
	static int __init parse_##var##_on(char *arg)			\
	{								\
		var = 1;						\
		return 0;						\
	}								\
	__setup_param(str_on, parse_##var##_on, parse_##var##_on, 1);	\
									\
	static int __init parse_##var##_off(char *arg)			\
	{								\
		var = 0;						\
		return 0;						\
	}								\
	__setup_param(str_off, parse_##var##_off, parse_##var##_off, 1)

/*依赖于 boot_command_line 的设置*/
void __init parse_early_param(void);
void __init parse_early_options(char *cmdline);
#endif /* __ASSEMBLY__ */

#else /* MODULE */

#define __setup_param(str, unique_id, fn)	/* nothing */
#define __setup(str, func) 			/* nothing */
#endif

/*标记为不被软件挂起保存的数据 */
#define __nosavedata __section(.data..nosave)

#ifdef MODULE
#define __exit_p(x) x
#else
#define __exit_p(x) NULL
#endif

#endif /* _LINUX_INIT_H */
