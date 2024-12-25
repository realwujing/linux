/*
 *  Housekeeping management. Manage the targets for routine code that can run on
 *  any CPU: unbound workqueues, timers, kthreads and any offloadable work.
 *
 * Copyright (C) 2017 Red Hat, Inc., Frederic Weisbecker
 * Copyright (C) 2017-2018 SUSE, Frederic Weisbecker
 *
 */
#include "sched.h"

DEFINE_STATIC_KEY_FALSE(housekeeping_overriden);
EXPORT_SYMBOL_GPL(housekeeping_overriden);
static cpumask_var_t housekeeping_mask;
static unsigned int housekeeping_flags;

int housekeeping_any_cpu(enum hk_flags flags)
{
	if (static_branch_unlikely(&housekeeping_overriden))
		if (housekeeping_flags & flags)
			return cpumask_any_and(housekeeping_mask, cpu_online_mask);
	return smp_processor_id();
}
EXPORT_SYMBOL_GPL(housekeeping_any_cpu);

const struct cpumask *housekeeping_cpumask(enum hk_flags flags)
{
	if (static_branch_unlikely(&housekeeping_overriden))
		if (housekeeping_flags & flags)
			return housekeeping_mask;
	return cpu_possible_mask;
}
EXPORT_SYMBOL_GPL(housekeeping_cpumask);

void housekeeping_affine(struct task_struct *t, enum hk_flags flags)
{
	if (static_branch_unlikely(&housekeeping_overriden))
		if (housekeeping_flags & flags)
			set_cpus_allowed_ptr(t, housekeeping_mask);
}
EXPORT_SYMBOL_GPL(housekeeping_affine);

bool housekeeping_test_cpu(int cpu, enum hk_flags flags)
{
	if (static_branch_unlikely(&housekeeping_overriden))
		if (housekeeping_flags & flags)
			return cpumask_test_cpu(cpu, housekeeping_mask);
	return true;
}
EXPORT_SYMBOL_GPL(housekeeping_test_cpu);

void __init housekeeping_init(void)
{
	if (!housekeeping_flags)
		return;

	static_branch_enable(&housekeeping_overriden);

	if (housekeeping_flags & HK_FLAG_TICK)
		sched_tick_offload_init();

	/* We need at least one CPU to handle housekeeping work */
	WARN_ON_ONCE(cpumask_empty(housekeeping_mask));
}

static int __init housekeeping_setup(char *str, enum hk_flags flags)
{
	cpumask_var_t non_housekeeping_mask; // 定义非housekeeping的CPU掩码变量
	int err; // 定义错误码变量

	alloc_bootmem_cpumask_var(&non_housekeeping_mask); // 分配非housekeeping的CPU掩码变量的内存
	err = cpulist_parse(str, non_housekeeping_mask); // 解析CPU列表字符串并填充非housekeeping的CPU掩码
	if (err < 0 || cpumask_last(non_housekeeping_mask) >= nr_cpu_ids) { // 如果解析出错或CPU范围超出限制
		pr_warn("Housekeeping: nohz_full= or isolcpus= incorrect CPU range\n"); // 打印警告信息
		free_bootmem_cpumask_var(non_housekeeping_mask); // 释放非housekeeping的CPU掩码变量的内存
		return 0; // 返回0表示失败
	}

	if (!housekeeping_flags) { // 如果housekeeping标志未设置
		alloc_bootmem_cpumask_var(&housekeeping_mask); // 分配housekeeping的CPU掩码变量的内存
		cpumask_andnot(housekeeping_mask,
				   cpu_possible_mask, non_housekeeping_mask); // 计算housekeeping的CPU掩码
		if (cpumask_empty(housekeeping_mask)) // 如果housekeeping的CPU掩码为空
			cpumask_set_cpu(smp_processor_id(), housekeeping_mask); // 设置当前CPU为housekeeping CPU
	} else { // 如果housekeeping标志已设置
		cpumask_var_t tmp; // 定义临时CPU掩码变量

		alloc_bootmem_cpumask_var(&tmp); // 分配临时CPU掩码变量的内存
		cpumask_andnot(tmp, cpu_possible_mask, non_housekeeping_mask); // 计算临时CPU掩码
		if (!cpumask_equal(tmp, housekeeping_mask)) { // 如果临时CPU掩码与housekeeping的CPU掩码不相等
			pr_warn("Housekeeping: nohz_full= must match isolcpus=\n"); // 打印警告信息
			free_bootmem_cpumask_var(tmp); // 释放临时CPU掩码变量的内存
			free_bootmem_cpumask_var(non_housekeeping_mask); // 释放非housekeeping的CPU掩码变量的内存
			return 0; // 返回0表示失败
		}
		free_bootmem_cpumask_var(tmp); // 释放临时CPU掩码变量的内存
	}

	if ((flags & HK_FLAG_TICK) && !(housekeeping_flags & HK_FLAG_TICK)) { // 如果设置了HK_FLAG_TICK标志但housekeeping标志未设置
		if (IS_ENABLED(CONFIG_NO_HZ_FULL)) { // 如果启用了CONFIG_NO_HZ_FULL配置
			tick_nohz_full_setup(non_housekeeping_mask); // 设置tick nohz full
		} else { // 如果未启用CONFIG_NO_HZ_FULL配置
			pr_warn("Housekeeping: nohz unsupported."
				" Build with CONFIG_NO_HZ_FULL\n"); // 打印警告信息
			free_bootmem_cpumask_var(non_housekeeping_mask); // 释放非housekeeping的CPU掩码变量的内存
			return 0; // 返回0表示失败
		}
	}

	housekeeping_flags |= flags; // 设置housekeeping标志

	free_bootmem_cpumask_var(non_housekeeping_mask); // 释放非housekeeping的CPU掩码变量的内存

	return 1; // 返回1表示成功
}

static int __init housekeeping_nohz_full_setup(char *str)
{
	unsigned int flags;

	flags = HK_FLAG_TICK | HK_FLAG_WQ | HK_FLAG_TIMER | HK_FLAG_RCU | HK_FLAG_MISC;

	return housekeeping_setup(str, flags);
}
__setup("nohz_full=", housekeeping_nohz_full_setup);

static int __init housekeeping_isolcpus_setup(char *str)
{
	unsigned int flags = 0;

	while (isalpha(*str)) {
		if (!strncmp(str, "nohz,", 5)) {
			str += 5;
			flags |= HK_FLAG_TICK;
			continue;
		}

		if (!strncmp(str, "domain,", 7)) {
			str += 7;
			flags |= HK_FLAG_DOMAIN;
			continue;
		}

		pr_warn("isolcpus: Error, unknown flag\n");
		return 0;
	}

	/* Default behaviour for isolcpus without flags */
	if (!flags)
		flags |= HK_FLAG_DOMAIN;

	return housekeeping_setup(str, flags);
}
__setup("isolcpus=", housekeeping_isolcpus_setup);
