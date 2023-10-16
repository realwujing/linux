/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  GK 2/5/95  -  Changed to support mounting root fs via NFS
 *  Added initrd & change_root: Werner Almesberger & Hans Lermen, Feb '96
 *  Moan early if gcc is old, avoiding bogus kernels - Paul Gortmaker, May '96
 *  Simplified starting of init:  Michael A. Griffith <grif@acm.org>
 */

#define DEBUG		/* Enable initcall_debug */

#include <linux/types.h>
#include <linux/extable.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/binfmts.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/bootmem.h>
#include <linux/acpi.h>
#include <linux/console.h>
#include <linux/nmi.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/rcupdate.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/cgroup.h>
#include <linux/efi.h>
#include <linux/tick.h>
#include <linux/sched/isolation.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/utsname.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/buffer_head.h>
#include <linux/page_ext.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/pid_namespace.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/init.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/sfi.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/ptrace.h>
#include <linux/pti.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/context_tracking.h>
#include <linux/random.h>
#include <linux/list.h>
#include <linux/integrity.h>
#include <linux/proc_ns.h>
#include <linux/io.h>
#include <linux/cache.h>
#include <linux/rodata_test.h>
#include <linux/jump_label.h>
#include <linux/mem_encrypt.h>

#include <asm/io.h>
#include <asm/bugs.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

#define CREATE_TRACE_POINTS
#include <trace/events/initcall.h>

static int kernel_init(void *);

extern void init_IRQ(void);
extern void fork_init(void);
extern void radix_tree_init(void);

/*
 * Debug helper: via this flag we know that we are in 'early bootup code'
 * where only the boot processor is running with IRQ disabled.  This means
 * two things - IRQ must not be enabled before the flag is cleared and some
 * operations which are not allowed with IRQ disabled are allowed while the
 * flag is set.
 */
bool early_boot_irqs_disabled __read_mostly;

enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

/*
 * Boot command-line arguments
 */
#define MAX_INIT_ARGS CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS CONFIG_INIT_ENV_ARG_LIMIT

extern void time_init(void);
/* Default late time init is NULL. archs can override this later. */
void (*__initdata late_time_init)(void);

/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* Untouched saved command line (eg. for /proc) */
char *saved_command_line;
/* Command line for parameter parsing */
static char *static_command_line;
/* Command line for per-initcall parameter parsing */
static char *initcall_command_line;

static char *execute_command;
static char *ramdisk_execute_command;

/*
 * Used to generate warnings if static_key manipulation functions are used
 * before jump_label_init is called.
 */
bool static_key_initialized __read_mostly;
EXPORT_SYMBOL_GPL(static_key_initialized);

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situation where previous kernel has crashed, BIOS has been
 * skipped and devices will be in unknown state.
 */
unsigned int reset_devices;
EXPORT_SYMBOL(reset_devices);

static int __init set_reset_devices(char *str)
{
	reset_devices = 1;
	return 1;
}

__setup("reset_devices", set_reset_devices);

static const char *argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char *envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

extern const struct obs_kernel_param __setup_start[], __setup_end[];

static bool __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	bool had_early_param = false;

	p = __setup_start;
	do {
		int n = strlen(p->str);
		if (parameqn(line, p->str, n)) {
			if (p->early) {
				/* Already done in parse_early_param?
				 * (Needs exact match on param part).
				 * Keep iterating, as we can have early
				 * params and __setups of same names 8( */
				if (line[n] == '\0' || line[n] == '=')
					had_early_param = true;
			} else if (!p->setup_func) {
				pr_warn("Parameter %s is obsolete, ignored\n",
					p->str);
				return true;
			} else if (p->setup_func(line + n))
				return true;
		}
		p++;
	} while (p < __setup_end);

	return had_early_param;
}

/*
 * This should be approx 2 Bo*oMips to start (note initial shift), and will
 * still work even if initially too large, it will just take slightly longer
 */
unsigned long loops_per_jiffy = (1<<12);
EXPORT_SYMBOL(loops_per_jiffy);

static int __init debug_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_DEBUG;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_QUIET;
	return 0;
}

early_param("debug", debug_kernel);
early_param("quiet", quiet_kernel);

static int __init loglevel(char *str)
{
	int newlevel;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &newlevel)) {
		console_loglevel = newlevel;
		return 0;
	}

	return -EINVAL;
}

early_param("loglevel", loglevel);

/* Change NUL term back to "=", to make "param" the whole string. */
static int __init repair_env_string(char *param, char *val,
				    const char *unused, void *arg)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
			val--;
		} else
			BUG();
	}
	return 0;
}

/* Anything after -- gets handed straight to init. */
static int __init set_init_arg(char *param, char *val,
			       const char *unused, void *arg)
{
	unsigned int i;

	if (panic_later)
		return 0;

	repair_env_string(param, val, unused, NULL);

	for (i = 0; argv_init[i]; i++) {
		if (i == MAX_INIT_ARGS) {
			panic_later = "init";
			panic_param = param;
			return 0;
		}
	}
	argv_init[i] = param;
	return 0;
}

/*
 * Unknown boot options get handed to init, unless they look like
 * unused parameters (modprobe will find them in /proc/cmdline).
 */
static int __init unknown_bootoption(char *param, char *val,
				     const char *unused, void *arg)
{
	repair_env_string(param, val, unused, NULL);

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strchr(param, '.') && (!val || strchr(param, '.') < val))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option */
		unsigned int i;
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "env";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], val - param))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option */
		unsigned int i;
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "init";
				panic_param = param;
			}
		}
		argv_init[i] = param;
	}
	return 0;
}

static int __init init_setup(char *str)
{
	unsigned int i;

	execute_command = str;
	/*
	 * In case LILO is going to boot us with default command line,
	 * it prepends "auto" before the whole cmdline which makes
	 * the shell think it should execute a script with such name.
	 * So we ignore all arguments entered _before_ init=... [MJ]
	 */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("init=", init_setup);

static int __init rdinit_setup(char *str)
{
	unsigned int i;

	ramdisk_execute_command = str;
	/* See "auto" comment in init_setup */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("rdinit=", rdinit_setup);

#ifndef CONFIG_SMP
static const unsigned int setup_max_cpus = NR_CPUS;
static inline void setup_nr_cpu_ids(void) { }
static inline void smp_prepare_cpus(unsigned int maxcpus) { }
#endif

/*
 * We need to store the untouched command line for future reference.
 * We also need to store the touched command line since the parameter
 * parsing is performed in place, and we should allow a component to
 * store reference of name/value for future reference.
 */
static void __init setup_command_line(char *command_line)
{
	saved_command_line =
		memblock_virt_alloc(strlen(boot_command_line) + 1, 0);
	initcall_command_line =
		memblock_virt_alloc(strlen(boot_command_line) + 1, 0);
	static_command_line = memblock_virt_alloc(strlen(command_line) + 1, 0);
	strcpy(saved_command_line, boot_command_line);
	strcpy(static_command_line, command_line);
}

/*
 * We need to finalize in a non-__init function or else race conditions
 * between the root thread and the init thread may cause start_kernel to
 * be reaped by free_initmem before the root thread has proceeded to
 * cpu_idle.
 *
 * gcc-3.4 accidentally inlines this function, so use noinline.
 */

static __initdata DECLARE_COMPLETION(kthreadd_done);

static noinline void __ref rest_init(void)
{
	struct task_struct *tsk;
	int pid;

	rcu_scheduler_starting();  // 启动 RCU 调度器

	/*
	 * 我们需要首先生成 init 进程，以便它获得 PID 1，但是
	 * init 任务最终会想要创建内核线程，如果在创建 kthreadd 之前
	 * 调度它，将导致 OOPS。
	 */
	pid = kernel_thread(kernel_init, NULL, CLONE_FS);

	/*
	 * 在引导 CPU 上钉住 init 进程。在运行 sched_init_smp() 之前，
	 * 任务迁移尚未正常工作。它将为 init 设置非隔离 CPU 的允许 CPU。
	 */
	rcu_read_lock();
	tsk = find_task_by_pid_ns(pid, &init_pid_ns);
	set_cpus_allowed_ptr(tsk, cpumask_of(smp_processor_id()));
	rcu_read_unlock();

	numa_default_policy();

	pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);
	rcu_read_lock();
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();

	/*
	 * 启用 might_sleep() 和 smp_processor_id() 检查。
	 * 它们不能在更早启用，因为对于 CONFIG_PREEMPT=y，
	 * kernel_thread() 将触发 might_sleep() 的 splat。对于
	 * CONFIG_PREEMPT_VOLUNTARY=y，init 任务可能已经调度，
	 * 但它卡在 kthreadd_done 完成上。
	 */
	system_state = SYSTEM_SCHEDULING;

	complete(&kthreadd_done);

	/*
	 * 引导空闲线程必须至少执行一次 schedule() 以启动进程：
	 */
	schedule_preempt_disabled();
	/* 在禁用抢占的情况下调用 cpu_idle */
	cpu_startup_entry(CPUHP_ONLINE);
}

/* Check for early params. */
static int __init do_early_param(char *param, char *val,
				 const char *unused, void *arg)
{
	const struct obs_kernel_param *p;

	for (p = __setup_start; p < __setup_end; p++) {
		if ((p->early && parameq(param, p->str)) ||
		    (strcmp(param, "console") == 0 &&
		     strcmp(p->str, "earlycon") == 0)
		) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

void __init parse_early_options(char *cmdline)
{
	parse_args("early options", cmdline, NULL, 0, 0, 0, NULL,
		   do_early_param);
}

/* Arch code calls this early on, or if not, just before other parsing. */
void __init parse_early_param(void)
{
	static int done __initdata;
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

	if (done)
		return;

	/* All fall through to do_early_param. */
	strlcpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
}

void __init __weak arch_post_acpi_subsys_init(void) { }

void __init __weak smp_setup_processor_id(void)
{
}

# if THREAD_SIZE >= PAGE_SIZE
void __init __weak thread_stack_cache_init(void)
{
}
#endif

void __init __weak mem_encrypt_init(void) { }

bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void);
#else
static inline void initcall_debug_enable(void)
{
}
#endif

/*
 * Set up kernel memory allocators
 */
static void __init mm_init(void)
{
	/*
	 * page_ext requires contiguous pages,
	 * bigger than MAX_ORDER unless SPARSEMEM.
	 */
	page_ext_init_flatmem();
	mem_init();
	kmem_cache_init();
	pgtable_init();
	vmalloc_init();
	ioremap_huge_init();
	/* Should be run before the first non-init thread is created */
	init_espfix_bsp();
	/* Should be run after espfix64 is set up. */
	pti_init();
}

asmlinkage __visible void __init start_kernel(void)
{
	char *command_line;  // 用于存储命令行参数
	char *after_dashes;  // 用于分隔命令行参数

	set_task_stack_end_magic(&init_task);  // 设置任务栈结束标志
	smp_setup_processor_id();  // 设置处理器ID
	debug_objects_early_init();  // 初始化调试对象

	cgroup_init_early();  // 提前初始化cgroup

	local_irq_disable();  // 禁用本地中断
	early_boot_irqs_disabled = true;  // 设置早期引导中断禁用标志

	// 还未启用中断。执行必要的设置，然后启用中断。
	boot_cpu_init();  // 初始化引导CPU
	page_address_init();  // 初始化页面地址
	pr_notice("%s", linux_banner);  // 打印内核版本信息
	setup_arch(&command_line);  // 设置体系结构相关信息
	// 设置初始canary和熵，包括架构、潜在和命令行熵
	add_latent_entropy();  // 增加随机性
	add_device_randomness(command_line, strlen(command_line));  // 增加设备相关随机性
	boot_init_stack_canary();  // 初始化堆栈canary
	mm_init_cpumask(&init_mm);  // 初始化内存管理
	setup_command_line(command_line);  // 设置命令行参数
	setup_nr_cpu_ids();  // 设置CPU ID
	setup_per_cpu_areas();  // 设置每个CPU的区域
	smp_prepare_boot_cpu();	// 架构特定的引导CPU钩子
	boot_cpu_hotplug_init();  // 初始化CPU热插拔

	build_all_zonelists(NULL);  // 构建所有区域列表
	page_alloc_init();  // 初始化页面分配器

	pr_notice("Kernel command line: %s\n", boot_command_line);  // 打印内核命令行
	parse_early_param();  // 解析早期参数
	after_dashes = parse_args("Booting kernel",
				  static_command_line, __start___param,
				  __stop___param - __start___param,
				  -1, -1, NULL, &unknown_bootoption);  // 解析命令行参数
	if (!IS_ERR_OR_NULL(after_dashes))
		parse_args("Setting init args", after_dashes, NULL, 0, -1, -1,
			   NULL, set_init_arg);  // 设置初始化参数

	jump_label_init();  // 初始化跳转标签

	// 这些使用大量的引导内存分配，必须在kmem_cache_init()之前执行
	setup_log_buf(0);  // 设置日志缓冲区
	vfs_caches_init_early();  // 提前初始化VFS缓存
	sort_main_extable();  // 对主扩展表排序
	trap_init();  // 初始化陷阱
	mm_init();  // 初始化内存管理子系统

	ftrace_init();  // 初始化跟踪
	// 在这里可以启用trace_printk
	early_trace_init();  // 提前初始化跟踪

	// 在启动任何中断之前设置调度器（如定时器中断）
	sched_init();  // 初始化调度器
	// 禁用抢占 - 早期引导调度非常脆弱，直到第一次cpu_idle()
	preempt_disable();
	if (WARN(!irqs_disabled(),
		 "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();  // 禁用中断
	radix_tree_init();  // 初始化基数树

	// 在设置工作队列之前设置维护，以便非维护的工作队列能够考虑进来
	housekeeping_init();  // 初始化系统维护工作

	// 允许提前创建工作队列和工作项排队/取消排队
	workqueue_init_early();  // 提前初始化工作队列

	rcu_init();  // 初始化RCU（读拷贝更新）机制

	// 在此之后可以使用跟踪事件
	trace_init();  // 初始化跟踪事件
	if (initcall_debug)
		initcall_debug_enable();

	context_tracking_init();  // 初始化上下文跟踪
	// 在初始化ISA IRQ之前初始化某些链接
	early_irq_init();
	init_IRQ();  // 初始化中断请求
	tick_init();  // 初始化时钟
	rcu_init_nohz();  // 初始化无中断的RCU
	init_timers();  // 初始化定时器
	hrtimers_init();  // 初始化高精度定时器
	softirq_init();  // 初始化软中断
	timekeeping_init();  // 初始化时间管理
	time_init();  // 初始化时间
	printk_safe_init();  // 初始化安全的打印
	perf_event_init();  // 初始化性能事件
	profile_init();  // 初始化性能分析
	call_function_init();  // 初始化调用函数
	if (WARN(!irqs_disabled(), "Interrupts were enabled early\n"))
	// 早期引导中断已禁用
	early_boot_irqs_disabled = false;
	local_irq_enable();  // 启用本地中断

	kmem_cache_init_late();  // 启动内存缓存

	// 注意：这是早期阶段的“黑客警报”！在我们进行PCI设置等操作之前启用控制台，
	// 而且console_init()必须知道这一点。但是我们确实希望尽早输出信息，以防出现问题。
	console_init();  // 初始化控制台
	if (panic_later)
		panic("Too many boot %s vars at `%s'", panic_later,
		      panic_param);  // 内核恐慌处理

	lockdep_init();  // 初始化锁依赖检查

	// 需要在启用IRQ的情况下运行此操作，因为它希望自测试[硬件/软件]-irqs打开/关闭锁反转错误
	locking_selftest();  // 锁测试

	// 在任何设备执行可能使用SWIOTLB弹出缓冲区的DMA操作之前必须运行此操作。
	// 它将标记弹出缓冲区为已解密，以使其使用时不会导致“明文”数据在访问时解密。
	mem_encrypt_init();  // 内存加密初始化

#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		pr_crit("initrd overwritten (0x%08lx < 0x%08lx) - disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);  // 初始化ramdisk被覆盖，禁用它
		initrd_start = 0;
	}
#endif
	page_ext_init();  // 初始化页扩展
	kmemleak_init();  // 初始化内存泄漏检测
	debug_objects_mem_init();  // 初始化调试对象的内存
	setup_per_cpu_pageset();  // 设置每CPU页面集
	numa_policy_init();  // 初始化NUMA策略
	acpi_early_init();  // ACPI早期初始化
	if (late_time_init)
		late_time_init();
	sched_clock_init();  // 初始化调度时钟
	calibrate_delay();  // 校准延迟
	pid_idr_init();  // 初始化PID分配器
	anon_vma_init();  // 初始化匿名VMA
#ifdef CONFIG_X86
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		efi_enter_virtual_mode();  // 进入EFI虚拟模式
#endif
	thread_stack_cache_init();  // 初始化线程栈缓存
	cred_init();  // 初始化凭据
	fork_init();  // 初始化进程
	proc_caches_init();  // 初始化进程缓存
	uts_ns_init();  // 初始化UTS命名空间
	buffer_init();  // 初始化缓冲区
	key_init();  // 初始化密钥
	security_init();  // 初始化安全性
	dbg_late_init();  // 调试模块的延迟初始化
	vfs_caches_init();  // 初始化VFS缓存
	pagecache_init();  // 初始化页面缓存
	signals_init();  // 初始化信号处理
	seq_file_init();  // 初始化序列文件
	proc_root_init();  // 初始化/proc根目录
	nsfs_init();  // 初始化命名空间文件系统
	cpuset_init();  // 初始化CPU集
	cgroup_init();  // 初始化控制组
	taskstats_init_early();  // 提前初始化任务统计
	delayacct_init();  // 初始化延迟帐户

	check_bugs();  // 检查BUG

	acpi_subsystem_init();  // ACPI子系统初始化
	arch_post_acpi_subsys_init();  // 架构ACPI子系统初始化
	sfi_init_late();  // 后期初始化SFI

	if (efi_enabled(EFI_RUNTIME_SERVICES)) {
		efi_free_boot_services();  // 释放EFI引导服务
	}

	// 其余的非__init'ed操作，现在我们活着了
	rest_init();  // 启动初始化
}

/* Call all constructor functions linked into the kernel. */
static void __init do_ctors(void)
{
#ifdef CONFIG_CONSTRUCTORS
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

#ifdef CONFIG_KALLSYMS
struct blacklist_entry {
	struct list_head next;
	char *buf;
};

static __initdata_or_module LIST_HEAD(blacklisted_initcalls);

static int __init initcall_blacklist(char *str)
{
	char *str_entry;
	struct blacklist_entry *entry;

	/* str argument is a comma-separated list of functions */
	do {
		str_entry = strsep(&str, ",");
		if (str_entry) {
			pr_debug("blacklisting initcall %s\n", str_entry);
			entry = alloc_bootmem(sizeof(*entry));
			entry->buf = alloc_bootmem(strlen(str_entry) + 1);
			strcpy(entry->buf, str_entry);
			list_add(&entry->next, &blacklisted_initcalls);
		}
	} while (str_entry);

	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	struct blacklist_entry *entry;
	char fn_name[KSYM_SYMBOL_LEN];
	unsigned long addr;

	if (list_empty(&blacklisted_initcalls))
		return false;

	addr = (unsigned long) dereference_function_descriptor(fn);
	sprint_symbol_no_offset(fn_name, addr);

	/*
	 * fn will be "function_name [module_name]" where [module_name] is not
	 * displayed for built-in init functions.  Strip off the [module_name].
	 */
	strreplace(fn_name, ' ', '\0');

	list_for_each_entry(entry, &blacklisted_initcalls, next) {
		if (!strcmp(fn_name, entry->buf)) {
			pr_debug("initcall %s blacklisted\n", fn_name);
			return true;
		}
	}

	return false;
}
#else
static int __init initcall_blacklist(char *str)
{
	pr_warn("initcall_blacklist requires CONFIG_KALLSYMS\n");
	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	return false;
}
#endif
__setup("initcall_blacklist=", initcall_blacklist);

static __init_or_module void
trace_initcall_start_cb(void *data, initcall_t fn)
{
	ktime_t *calltime = (ktime_t *)data;

	printk(KERN_DEBUG "calling  %pF @ %i\n", fn, task_pid_nr(current));
	*calltime = ktime_get();
}

static __init_or_module void
trace_initcall_finish_cb(void *data, initcall_t fn, int ret)
{
	ktime_t *calltime = (ktime_t *)data;
	ktime_t delta, rettime;
	unsigned long long duration;

	rettime = ktime_get();
	delta = ktime_sub(rettime, *calltime);
	duration = (unsigned long long) ktime_to_ns(delta) >> 10;
	printk(KERN_DEBUG "initcall %pF returned %d after %lld usecs\n",
		 fn, ret, duration);
}

static ktime_t initcall_calltime;

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void)
{
	int ret;

	ret = register_trace_initcall_start(trace_initcall_start_cb,
					    &initcall_calltime);
	ret |= register_trace_initcall_finish(trace_initcall_finish_cb,
					      &initcall_calltime);
	WARN(ret, "Failed to register initcall tracepoints\n");
}
# define do_trace_initcall_start	trace_initcall_start
# define do_trace_initcall_finish	trace_initcall_finish
#else
static inline void do_trace_initcall_start(initcall_t fn)
{
	if (!initcall_debug)
		return;
	trace_initcall_start_cb(&initcall_calltime, fn);
}
static inline void do_trace_initcall_finish(initcall_t fn, int ret)
{
	if (!initcall_debug)
		return;
	trace_initcall_finish_cb(&initcall_calltime, fn, ret);
}
#endif /* !TRACEPOINTS_ENABLED */

int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	char msgbuf[64];
	int ret;

	if (initcall_blacklisted(fn))
		return -EPERM;

	do_trace_initcall_start(fn);
	ret = fn();
	do_trace_initcall_finish(fn, ret);

	msgbuf[0] = 0;

	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count);
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pF returned with %s\n", fn, msgbuf);

	add_latent_entropy();
	return ret;
}


extern initcall_entry_t __initcall_start[];
extern initcall_entry_t __initcall0_start[];
extern initcall_entry_t __initcall1_start[];
extern initcall_entry_t __initcall2_start[];
extern initcall_entry_t __initcall3_start[];
extern initcall_entry_t __initcall4_start[];
extern initcall_entry_t __initcall5_start[];
extern initcall_entry_t __initcall6_start[];
extern initcall_entry_t __initcall7_start[];
extern initcall_entry_t __initcall_end[];

static initcall_entry_t *initcall_levels[] __initdata = {
	__initcall0_start,
	__initcall1_start,
	__initcall2_start,
	__initcall3_start,
	__initcall4_start,
	__initcall5_start,
	__initcall6_start,
	__initcall7_start,
	__initcall_end,
};

/* Keep these in sync with initcalls in include/linux/init.h */
static char *initcall_level_names[] __initdata = {
	"pure",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static void __init do_initcall_level(int level)
{
	initcall_entry_t *fn;

	strcpy(initcall_command_line, saved_command_line);
	parse_args(initcall_level_names[level],
		   initcall_command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   NULL, &repair_env_string);
	trace_initcall_level(initcall_level_names[level]);  // 跟踪初始化级别
	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(initcall_from_entry(fn));  // 执行一个初始化函数
}

static void __init do_initcalls(void)
{
	int level;

	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++)
		do_initcall_level(level);  // 执行特定级别的初始化函数
}

/*
 * 现在，机器已经初始化。尽管还没有触及设备，但 CPU 子系统已经运行起来，
 * 内存和进程管理也能正常工作。
 *
 * 现在，我们终于可以开始做一些真正的工作了...
 */
static void __init do_basic_setup(void)
{
	cpuset_init_smp();  // 初始化 CPU 集合（SMP 模式）

	shmem_init();  // 初始化共享内存文件系统

	driver_init();  // 初始化驱动程序子系统

	init_irq_proc();  // 初始化 IRQ 处理过程

	do_ctors();  // 执行构造函数

	usermodehelper_enable();  // 启用用户态帮助程序

	do_initcalls();  // 执行初始化函数调用
}

static void __init do_pre_smp_initcalls(void)
{
	initcall_entry_t *fn;

	trace_initcall_level("early");
	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

/*
 * This function requests modules which should be loaded by default and is
 * called twice right after initrd is mounted and right before init is
 * exec'd.  If such modules are on either initrd or rootfs, they will be
 * loaded before control is passed to userland.
 */
void __init load_default_modules(void)
{
	load_default_elevator_module();
}

static int run_init_process(const char *init_filename)
{
	argv_init[0] = init_filename;
	pr_info("Run %s as init process\n", init_filename);
	return do_execve(getname_kernel(init_filename),
		(const char __user *const __user *)argv_init,
		(const char __user *const __user *)envp_init);
}

static int try_to_run_init_process(const char *init_filename)
{
	int ret;

	ret = run_init_process(init_filename);

	if (ret && ret != -ENOENT) {
		pr_err("Starting init: %s exists but couldn't execute it (error %d)\n",
		       init_filename, ret);
	}

	return ret;
}

static noinline void __init kernel_init_freeable(void);

#if defined(CONFIG_STRICT_KERNEL_RWX) || defined(CONFIG_STRICT_MODULE_RWX)
bool rodata_enabled __ro_after_init = true;
static int __init set_debug_rodata(char *str)
{
	return strtobool(str, &rodata_enabled);
}
__setup("rodata=", set_debug_rodata);
#endif

#ifdef CONFIG_STRICT_KERNEL_RWX
static void mark_readonly(void)
{
	if (rodata_enabled) {
		/*
		 * load_module() results in W+X mappings, which are cleaned up
		 * with call_rcu_sched().  Let's make sure that queued work is
		 * flushed so that we don't hit false positives looking for
		 * insecure pages which are W+X.
		 */
		rcu_barrier_sched();
		mark_rodata_ro();
		rodata_test();
	} else
		pr_info("Kernel memory protection disabled.\n");
}
#else
static inline void mark_readonly(void)
{
	pr_warn("This architecture does not have kernel memory protection.\n");
}
#endif

static int __ref kernel_init(void *unused)
{
	int ret;

	kernel_init_freeable();  // 初始化内核的可释放部分

	/* 需要在释放内存之前完成所有异步 __init 代码 */
	async_synchronize_full();  // 异步同步操作，确保所有异步初始化代码完成

	ftrace_free_init_mem();  // 释放 ftrace 的初始化内存

	jump_label_invalidate_initmem();  // 使初始化内存中的跳转标签失效

	free_initmem();  // 释放初始化内存

	mark_readonly();  // 标记初始化内存为只读

	/*
	 * 现在已经确定内核映射 - 更新用户空间页表以完成 PTI。
	 */
	pti_finalize();  // 完成 PTI (Page Table Isolation) 的最终化

	system_state = SYSTEM_RUNNING;  // 设置系统状态为运行中

	numa_default_policy();  // 设置 NUMA (Non-Uniform Memory Access) 默认策略

	rcu_end_inkernel_boot();  // 结束内核引导中的 RCU (Read-Copy Update)

	if (ramdisk_execute_command) {
		ret = run_init_process(ramdisk_execute_command);
		if (!ret)
			return 0;
		pr_err("Failed to execute %s (error %d)\n",
		       ramdisk_execute_command, ret);
	}

	/*
	 * 我们尝试每个 init 进程，直到一个成功为止。
	 *
	 * 如果我们尝试修复一个非常损坏的机器，可以使用 Bourne shell 代替 init。
	 */
	if (execute_command) {
		ret = run_init_process(execute_command);
		if (!ret)
			return 0;
		panic("Requested init %s failed (error %d).",
		      execute_command, ret);
	}

	if (!try_to_run_init_process("/sbin/init") ||
	    !try_to_run_init_process("/etc/init") ||
	    !try_to_run_init_process("/bin/init") ||
	    !try_to_run_init_process("/bin/sh"))
		return 0;

	panic("No working init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/admin-guide/init.rst for guidance.");
}

static noinline void __init kernel_init_freeable(void)
{
	/*
	 * 等待直到 kthreadd 完全设置好。
	 */
	wait_for_completion(&kthreadd_done);  // 等待 kthreadd 完成初始化

	/* 现在调度程序完全设置好，可以执行阻塞分配 */
	gfp_allowed_mask = __GFP_BITS_MASK;  // 允许所有 GFP 标志位

	/*
	 * init 进程可以在任何节点上分配页面
	 */
	set_mems_allowed(node_states[N_MEMORY]);  // 设置允许 init 进程在所有节点上分配内存

	cad_pid = task_pid(current);  // 获取当前任务（init 进程）的 PID

	smp_prepare_cpus(setup_max_cpus);  // 准备多核 CPU 的配置

	workqueue_init();  // 初始化工作队列

	init_mm_internals();  // 初始化 init 进程的 mm 结构

	do_pre_smp_initcalls();  // 执行 SMP 模式下的预初始化调用

	lockup_detector_init();  // 初始化锁死检测器

	smp_init();  // 初始化 SMP 相关功能

	sched_init_smp();  // 初始化 SMP 模式下的调度器

	page_alloc_init_late();  // 初始化页面分配（稍后）

	do_basic_setup();  // 执行基本的系统初始化

	/* 打开 /dev/console，这应该永远不会失败 */
	if (ksys_open((const char __user *) "/dev/console", O_RDWR, 0) < 0)
		pr_err("Warning: unable to open an initial console.\n");  // 打开控制台设备，如果失败，发出警告

	(void) ksys_dup(0);  // 复制标准输入描述符
	(void) ksys_dup(0);  // 再次复制标准输入描述符
	/*
	 * 检查是否有早期的用户空间初始化。如果有，让它完成所有工作
	 */

	if (!ramdisk_execute_command)
		ramdisk_execute_command = "/init";  // 如果没有指定初始化命令，使用默认的 "/init"

	if (ksys_access((const char __user *)
			ramdisk_execute_command, 0) != 0) {
		ramdisk_execute_command = NULL;  // 如果指定的初始化命令不可访问，清除它
		prepare_namespace();  // 准备命名空间
	}

	/*
	 * 好的，我们已经完成了初始引导过程，基本上已经启动并运行。
	 * 现在，去掉 initmem 段，并启动用户模式的进程。
	 *
	 * rootfs 现在可用，尝试加载公钥和默认模块
	 */

	integrity_load_keys();  // 加载完整性验证的公钥

	load_default_modules();  // 加载默认内核模块
}
