/*
 * SMP initialisation and IPI support
 * Based on arch/arm/kernel/smp.c
 *
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/cache.h>
#include <linux/profile.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/seq_file.h>
#include <linux/irq.h>
#include <linux/percpu.h>
#include <linux/clockchips.h>
#include <linux/completion.h>
#include <linux/of.h>
#include <linux/irq_work.h>

#include <asm/alternative.h>
#include <asm/atomic.h>
#include <asm/cacheflush.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/cpu_ops.h>
#include <asm/mmu_context.h>
#include <asm/numa.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/processor.h>
#include <asm/smp_plat.h>
#include <asm/sections.h>
#include <asm/tlbflush.h>
#include <asm/ptrace.h>
#include <asm/virt.h>
#include <soc/qcom/lpm_levels.h>
#include <soc/qcom/minidump.h>

#define CREATE_TRACE_POINTS
#include <trace/events/ipi.h>

DEFINE_PER_CPU_READ_MOSTLY(int, cpu_number);
EXPORT_PER_CPU_SYMBOL(cpu_number);

/*
 * as from 2.5, kernels no longer have an init_tasks structure
 * so we need some other way of telling a new secondary core
 * where to place its SVC stack
 */
struct secondary_data secondary_data;
/* Number of CPUs which aren't online, but looping in kernel text. */
int cpus_stuck_in_kernel;

enum ipi_msg_type {
	IPI_RESCHEDULE,
	IPI_CALL_FUNC,
	IPI_CPU_STOP,
	IPI_TIMER,
	IPI_IRQ_WORK,
	IPI_WAKEUP,
	IPI_CPU_BACKTRACE,
};

#ifdef CONFIG_ARM64_VHE

/* Whether the boot CPU is running in HYP mode or not*/
static bool boot_cpu_hyp_mode;

static inline void save_boot_cpu_run_el(void)
{
	boot_cpu_hyp_mode = is_kernel_in_hyp_mode();
}

static inline bool is_boot_cpu_in_hyp_mode(void)
{
	return boot_cpu_hyp_mode;
}

/*
 * Verify that a secondary CPU is running the kernel at the same
 * EL as that of the boot CPU.
 */
void verify_cpu_run_el(void)
{
	bool in_el2 = is_kernel_in_hyp_mode();
	bool boot_cpu_el2 = is_boot_cpu_in_hyp_mode();

	if (in_el2 ^ boot_cpu_el2) {
		pr_crit("CPU%d: mismatched Exception Level(EL%d) with boot CPU(EL%d)\n",
					smp_processor_id(),
					in_el2 ? 2 : 1,
					boot_cpu_el2 ? 2 : 1);
		cpu_panic_kernel();
	}
}

#else
static inline void save_boot_cpu_run_el(void) {}
#endif

#ifdef CONFIG_HOTPLUG_CPU
static int op_cpu_kill(unsigned int cpu);
#else
static inline int op_cpu_kill(unsigned int cpu)
{
	return -ENOSYS;
}
#endif


/*
 * Boot a secondary CPU, and assign it the specified idle task.
 * This also gives us the initial stack to use for this CPU.
 */
static int boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	if (cpu_ops[cpu]->cpu_boot)
		return cpu_ops[cpu]->cpu_boot(cpu);

	return -EOPNOTSUPP;
}

static DECLARE_COMPLETION(cpu_running);
bool va52mismatch __ro_after_init;

int __cpu_up(unsigned int cpu, struct task_struct *idle)
{
	int ret;
	long status;

	/*
	 * We need to tell the secondary core where to find its stack and the
	 * page tables.
	 */
	secondary_data.task = idle;
	secondary_data.stack = task_stack_page(idle) + THREAD_START_SP;
	update_cpu_boot_status(CPU_MMU_OFF);
	__flush_dcache_area(&secondary_data, sizeof(secondary_data));

	/*
	 * Now bring the CPU into our world.
	 */
	ret = boot_secondary(cpu, idle);
	if (ret == 0) {
		/*
		 * CPU was successfully started, wait for it to come online or
		 * time out.
		 */
		wait_for_completion_timeout(&cpu_running,
					    msecs_to_jiffies(1000));

		if (!cpu_online(cpu)) {
			pr_crit("CPU%u: failed to come online\n", cpu);

			if (IS_ENABLED(CONFIG_ARM64_52BIT_VA) && va52mismatch)
				pr_crit("CPU%u: does not support 52-bit VAs\n", cpu);

			ret = -EIO;
		}
	} else {
		pr_err("CPU%u: failed to boot: %d\n", cpu, ret);
		return ret;
	}

	secondary_data.task = NULL;
	secondary_data.stack = NULL;
	status = READ_ONCE(secondary_data.status);
	if (ret && status) {

		if (status == CPU_MMU_OFF)
			status = READ_ONCE(__early_cpu_boot_status);

		switch (status) {
		default:
			pr_err("CPU%u: failed in unknown state : 0x%lx\n",
					cpu, status);
			break;
		case CPU_KILL_ME:
			if (!op_cpu_kill(cpu)) {
				pr_crit("CPU%u: died during early boot\n", cpu);
				break;
			}
			/* Fall through */
			pr_crit("CPU%u: may not have shut down cleanly\n", cpu);
		case CPU_STUCK_IN_KERNEL:
			pr_crit("CPU%u: is stuck in kernel\n", cpu);
			cpus_stuck_in_kernel++;
			break;
		case CPU_PANIC_KERNEL:
			panic("CPU%u detected unsupported configuration\n", cpu);
		}
	}

	return ret;
}

/*
 * This is the secondary CPU boot entry.  We're using this CPUs
 * idle thread stack, but a set of temporary page tables.
 */
asmlinkage notrace void secondary_start_kernel(void)
{
	struct mm_struct *mm = &init_mm;
	unsigned int cpu;

	cpu = task_cpu(current);
	set_my_cpu_offset(per_cpu_offset(cpu));

	/*
	 * All kernel threads share the same mm context; grab a
	 * reference and switch to it.
	 */
	atomic_inc(&mm->mm_count);
	current->active_mm = mm;

//	pr_debug("CPU%u: Booted secondary processor\n", cpu);

	/*
	 * TTBR0 is only used for the identity mapping at this stage. Make it
	 * point to zero page to avoid speculatively fetching new entries.
	 */
	cpu_uninstall_idmap();

	preempt_disable();
	trace_hardirqs_off();

	/*
	 * If the system has established the capabilities, make sure
	 * this CPU ticks all of those. If it doesn't, the CPU will
	 * fail to come online.
	 */
	check_local_cpu_capabilities();

	if (cpu_ops[cpu]->cpu_postboot)
		cpu_ops[cpu]->cpu_postboot();

	/*
	 * Log the CPU info before it is marked online and might get read.
	 */
	cpuinfo_store_cpu();

	/*
	 * Enable GIC and timers.
	 */
	notify_cpu_starting(cpu);

	store_cpu_topology(cpu);

	/*
	 * OK, now it's safe to let the boot CPU continue.  Wait for
	 * the CPU migration code to notice that the CPU is online
	 * before we continue.
	 */
//	pr_debug("CPU%u: Booted secondary processor [%08x]\n",
//					 cpu, read_cpuid_id());
	update_cpu_boot_status(CPU_BOOT_SUCCESS);
	set_cpu_online(cpu, true);
	complete(&cpu_running);

	local_irq_enable();
	local_async_enable();

	/*
	 * OK, it's off to the idle thread for us
	 */
	cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
}

#ifdef CONFIG_HOTPLUG_CPU
static int op_cpu_disable(unsigned int cpu)
{
	/*
	 * If we don't have a cpu_die method, abort before we reach the point
	 * of no return. CPU0 may not have an cpu_ops, so test for it.
	 */
	if (!cpu_ops[cpu] || !cpu_ops[cpu]->cpu_die)
		return -EOPNOTSUPP;

	/*
	 * We may need to abort a hot unplug for some other mechanism-specific
	 * reason.
	 */
	if (cpu_ops[cpu]->cpu_disable)
		return cpu_ops[cpu]->cpu_disable(cpu);

	return 0;
}

/*
 * __cpu_disable runs on the processor to be shutdown.
 */
int __cpu_disable(void)
{
	unsigned int cpu = smp_processor_id();
	int ret;

	ret = op_cpu_disable(cpu);
	if (ret)
		return ret;

	/*
	 * Take this CPU offline.  Once we clear this, we can't return,
	 * and we must not schedule until we're ready to give up the cpu.
	 */
	set_cpu_online(cpu, false);

	/*
	 * OK - migrate IRQs away from this CPU
	 */
	irq_migrate_all_off_this_cpu();

	return 0;
}

static int op_cpu_kill(unsigned int cpu)
{
	/*
	 * If we have no means of synchronising with the dying CPU, then assume
	 * that it is really dead. We can only wait for an arbitrary length of
	 * time and hope that it's dead, so let's skip the wait and just hope.
	 */
	if (!cpu_ops[cpu]->cpu_kill)
		return 0;

	return cpu_ops[cpu]->cpu_kill(cpu);
}

/*
 * called on the thread which is asking for a CPU to be shutdown -
 * waits until shutdown has completed, or it is timed out.
 */
void __cpu_die(unsigned int cpu)
{
	int err;

	if (!cpu_wait_death(cpu, 5)) {
		pr_crit("CPU%u: cpu didn't die\n", cpu);
		return;
	}
	pr_debug("CPU%u: shutdown\n", cpu);

	/*
	 * Now that the dying CPU is beyond the point of no return w.r.t.
	 * in-kernel synchronisation, try to get the firwmare to help us to
	 * verify that it has really left the kernel before we consider
	 * clobbering anything it might still be using.
	 */
	err = op_cpu_kill(cpu);
	if (err)
		pr_warn("CPU%d may not have shut down cleanly: %d\n",
			cpu, err);
}

/*
 * Called from the idle thread for the CPU which has been shutdown.
 *
 * Note that we disable IRQs here, but do not re-enable them
 * before returning to the caller. This is also the behaviour
 * of the other hotplug-cpu capable cores, so presumably coming
 * out of idle fixes this.
 */
void cpu_die(void)
{
	unsigned int cpu = smp_processor_id();

	idle_task_exit();

	local_irq_disable();

	/* Tell __cpu_die() that this CPU is now safe to dispose of */
	(void)cpu_report_death();

	/*
	 * Actually shutdown the CPU. This must never fail. The specific hotplug
	 * mechanism must perform all required cache maintenance to ensure that
	 * no dirty lines are lost in the process of shutting down the CPU.
	 */
	cpu_ops[cpu]->cpu_die(cpu);

	BUG();
}
#endif

/*
 * Kill the calling secondary CPU, early in bringup before it is turned
 * online.
 */
void cpu_die_early(void)
{
	int cpu = smp_processor_id();

	pr_crit("CPU%d: will not boot\n", cpu);

	/* Mark this CPU absent */
	set_cpu_present(cpu, 0);

#ifdef CONFIG_HOTPLUG_CPU
	update_cpu_boot_status(CPU_KILL_ME);
	/* Check if we can park ourselves */
	if (cpu_ops[cpu] && cpu_ops[cpu]->cpu_die)
		cpu_ops[cpu]->cpu_die(cpu);
#endif
	update_cpu_boot_status(CPU_STUCK_IN_KERNEL);

	cpu_park_loop();
}

static void __init hyp_mode_check(void)
{
	if (is_hyp_mode_available())
		pr_info("CPU: All CPU(s) started at EL2\n");
	else if (is_hyp_mode_mismatched())
		WARN_TAINT(1, TAINT_CPU_OUT_OF_SPEC,
			   "CPU: CPUs started in inconsistent modes");
	else
		pr_info("CPU: All CPU(s) started at EL1\n");
}

void __init smp_cpus_done(unsigned int max_cpus)
{
	pr_info("SMP: Total of %d processors activated.\n", num_online_cpus());
	setup_cpu_features();
	hyp_mode_check();
	apply_alternatives_all();
}

void __init smp_prepare_boot_cpu(void)
{
	set_my_cpu_offset(per_cpu_offset(smp_processor_id()));
	/*
	 * Initialise the static keys early as they may be enabled by the
	 * cpufeature code.
	 */
	jump_label_init();
	cpuinfo_store_boot_cpu();
	save_boot_cpu_run_el();
}

static u64 __init of_get_cpu_mpidr(struct device_node *dn)
{
	const __be32 *cell;
	u64 hwid;

	/*
	 * A cpu node with missing "reg" property is
	 * considered invalid to build a cpu_logical_map
	 * entry.
	 */
	cell = of_get_property(dn, "reg", NULL);
	if (!cell) {
		pr_err("%s: missing reg property\n", dn->full_name);
		return INVALID_HWID;
	}

	hwid = of_read_number(cell, of_n_addr_cells(dn));
	/*
	 * Non affinity bits must be set to 0 in the DT
	 */
	if (hwid & ~MPIDR_HWID_BITMASK) {
		pr_err("%s: invalid reg property\n", dn->full_name);
		return INVALID_HWID;
	}
	return hwid;
}

/*
 * Duplicate MPIDRs are a recipe for disaster. Scan all initialized
 * entries and check for duplicates. If any is found just ignore the
 * cpu. cpu_logical_map was initialized to INVALID_HWID to avoid
 * matching valid MPIDR values.
 */
static bool __init is_mpidr_duplicate(unsigned int cpu, u64 hwid)
{
	unsigned int i;

	for (i = 1; (i < cpu) && (i < NR_CPUS); i++)
		if (cpu_logical_map(i) == hwid)
			return true;
	return false;
}

/*
 * Initialize cpu operations for a logical cpu and
 * set it in the possible mask on success
 */
static int __init smp_cpu_setup(int cpu)
{
	if (cpu_read_ops(cpu))
		return -ENODEV;

	if (cpu_ops[cpu]->cpu_init(cpu))
		return -ENODEV;

	set_cpu_possible(cpu, true);

	return 0;
}

static bool bootcpu_valid __initdata;
static unsigned int cpu_count = 1;

#ifdef CONFIG_ACPI
/*
 * acpi_map_gic_cpu_interface - parse processor MADT entry
 *
 * Carry out sanity checks on MADT processor entry and initialize
 * cpu_logical_map on success
 */
static void __init
acpi_map_gic_cpu_interface(struct acpi_madt_generic_interrupt *processor)
{
	u64 hwid = processor->arm_mpidr;

	if (!(processor->flags & ACPI_MADT_ENABLED)) {
		pr_debug("skipping disabled CPU entry with 0x%llx MPIDR\n", hwid);
		return;
	}

	if (hwid & ~MPIDR_HWID_BITMASK || hwid == INVALID_HWID) {
		pr_err("skipping CPU entry with invalid MPIDR 0x%llx\n", hwid);
		return;
	}

	if (is_mpidr_duplicate(cpu_count, hwid)) {
		pr_err("duplicate CPU MPIDR 0x%llx in MADT\n", hwid);
		return;
	}

	/* Check if GICC structure of boot CPU is available in the MADT */
	if (cpu_logical_map(0) == hwid) {
		if (bootcpu_valid) {
			pr_err("duplicate boot CPU MPIDR: 0x%llx in MADT\n",
			       hwid);
			return;
		}
		bootcpu_valid = true;
		early_map_cpu_to_node(0, acpi_numa_get_nid(0, hwid));
		return;
	}

	if (cpu_count >= NR_CPUS)
		return;

	/* map the logical cpu id to cpu MPIDR */
	cpu_logical_map(cpu_count) = hwid;

	/*
	 * Set-up the ACPI parking protocol cpu entries
	 * while initializing the cpu_logical_map to
	 * avoid parsing MADT entries multiple times for
	 * nothing (ie a valid cpu_logical_map entry should
	 * contain a valid parking protocol data set to
	 * initialize the cpu if the parking protocol is
	 * the only available enable method).
	 */
	acpi_set_mailbox_entry(cpu_count, processor);

	early_map_cpu_to_node(cpu_count, acpi_numa_get_nid(cpu_count, hwid));

	cpu_count++;
}

static int __init
acpi_parse_gic_cpu_interface(struct acpi_subtable_header *header,
			     const unsigned long end)
{
	struct acpi_madt_generic_interrupt *processor;

	processor = (struct acpi_madt_generic_interrupt *)header;
	if (BAD_MADT_GICC_ENTRY(processor, end))
		return -EINVAL;

	acpi_table_print_madt_entry(header);

	acpi_map_gic_cpu_interface(processor);

	return 0;
}
#else
#define acpi_table_parse_madt(...)	do { } while (0)
#endif
void (*__smp_cross_call)(const struct cpumask *, unsigned int);
DEFINE_PER_CPU(bool, pending_ipi);

/*
 * Enumerate the possible CPU set from the device tree and build the
 * cpu logical map array containing MPIDR values related to logical
 * cpus. Assumes that cpu_logical_map(0) has already been initialized.
 */
static void __init of_parse_and_init_cpus(void)
{
	struct device_node *dn = NULL;

	while ((dn = of_find_node_by_type(dn, "cpu"))) {
		u64 hwid = of_get_cpu_mpidr(dn);

		if (hwid == INVALID_HWID)
			goto next;

		if (is_mpidr_duplicate(cpu_count, hwid)) {
			pr_err("%s: duplicate cpu reg properties in the DT\n",
				dn->full_name);
			goto next;
		}

		/*
		 * The numbering scheme requires that the boot CPU
		 * must be assigned logical id 0. Record it so that
		 * the logical map built from DT is validated and can
		 * be used.
		 */
		if (hwid == cpu_logical_map(0)) {
			if (bootcpu_valid) {
				pr_err("%s: duplicate boot cpu reg property in DT\n",
					dn->full_name);
				goto next;
			}

			bootcpu_valid = true;
			early_map_cpu_to_node(0, of_node_to_nid(dn));

			/*
			 * cpu_logical_map has already been
			 * initialized and the boot cpu doesn't need
			 * the enable-method so continue without
			 * incrementing cpu.
			 */
			continue;
		}

		if (cpu_count >= NR_CPUS)
			goto next;

		pr_debug("cpu logical map 0x%llx\n", hwid);
		cpu_logical_map(cpu_count) = hwid;

		early_map_cpu_to_node(cpu_count, of_node_to_nid(dn));
next:
		cpu_count++;
	}
}

/*
 * Enumerate the possible CPU set from the device tree or ACPI and build the
 * cpu logical map array containing MPIDR values related to logical
 * cpus. Assumes that cpu_logical_map(0) has already been initialized.
 */
void __init smp_init_cpus(void)
{
	int i;

	if (acpi_disabled)
		of_parse_and_init_cpus();
	else
		/*
		 * do a walk of MADT to determine how many CPUs
		 * we have including disabled CPUs, and get information
		 * we need for SMP init
		 */
		acpi_table_parse_madt(ACPI_MADT_TYPE_GENERIC_INTERRUPT,
				      acpi_parse_gic_cpu_interface, 0);

	if (cpu_count > nr_cpu_ids)
		pr_warn("Number of cores (%d) exceeds configured maximum of %d - clipping\n",
			cpu_count, nr_cpu_ids);

	if (!bootcpu_valid) {
		pr_err("missing boot CPU MPIDR, not enabling secondaries\n");
		return;
	}

	/*
	 * We need to set the cpu_logical_map entries before enabling
	 * the cpus so that cpu processor description entries (DT cpu nodes
	 * and ACPI MADT entries) can be retrieved by matching the cpu hwid
	 * with entries in cpu_logical_map while initializing the cpus.
	 * If the cpu set-up fails, invalidate the cpu_logical_map entry.
	 */
	for (i = 1; i < nr_cpu_ids; i++) {
		if (cpu_logical_map(i) != INVALID_HWID) {
			if (smp_cpu_setup(i))
				cpu_logical_map(i) = INVALID_HWID;
		}
	}
}

void __init smp_prepare_cpus(unsigned int max_cpus)
{
	int err;
	unsigned int cpu;
	unsigned int this_cpu;

	init_cpu_topology();

	this_cpu = smp_processor_id();
	store_cpu_topology(this_cpu);
	numa_store_cpu_info(this_cpu);

	/*
	 * If UP is mandated by "nosmp" (which implies "maxcpus=0"), don't set
	 * secondary CPUs present.
	 */
	if (max_cpus == 0)
		return;

	/*
	 * Initialise the present map (which describes the set of CPUs
	 * actually populated at the present time) and release the
	 * secondaries from the bootloader.
	 */
	for_each_possible_cpu(cpu) {

		per_cpu(cpu_number, cpu) = cpu;

		if (cpu == smp_processor_id())
			continue;

		if (!cpu_ops[cpu])
			continue;

		err = cpu_ops[cpu]->cpu_prepare(cpu);
		if (err)
			continue;

		set_cpu_present(cpu, true);
		numa_store_cpu_info(cpu);
	}
}

void __init set_smp_cross_call(void (*fn)(const struct cpumask *, unsigned int))
{
	__smp_cross_call = fn;
}

static const char *ipi_types[NR_IPI] __tracepoint_string = {
#define S(x,s)	[x] = s
	S(IPI_RESCHEDULE, "Rescheduling interrupts"),
	S(IPI_CALL_FUNC, "Function call interrupts"),
	S(IPI_CPU_STOP, "CPU stop interrupts"),
	S(IPI_TIMER, "Timer broadcast interrupts"),
	S(IPI_IRQ_WORK, "IRQ work interrupts"),
	S(IPI_WAKEUP, "CPU wake-up interrupts"),
	S(IPI_CPU_BACKTRACE, "CPU backtrace"),
};

static void smp_cross_call(const struct cpumask *target, unsigned int ipinr)
{
	trace_ipi_raise(target, ipi_types[ipinr]);
	__smp_cross_call(target, ipinr);
}

static void smp_cross_call_common(const struct cpumask *cpumask,
				  unsigned int func)
{
	unsigned int cpu;

	for_each_cpu(cpu, cpumask)
		per_cpu(pending_ipi, cpu) = true;

	smp_cross_call(cpumask, func);
}

void show_ipi_list(struct seq_file *p, int prec)
{
	unsigned int cpu, i;

	for (i = 0; i < NR_IPI; i++) {
		seq_printf(p, "%*s%u:%s", prec - 1, "IPI", i,
			   prec >= 4 ? " " : "");
		for_each_online_cpu(cpu)
			seq_printf(p, "%10u ",
				   __get_irq_stat(cpu, ipi_irqs[i]));
		seq_printf(p, "      %s\n", ipi_types[i]);
	}
}

u64 smp_irq_stat_cpu(unsigned int cpu)
{
	u64 sum = 0;
	int i;

	for (i = 0; i < NR_IPI; i++)
		sum += __get_irq_stat(cpu, ipi_irqs[i]);

	return sum;
}

void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	smp_cross_call_common(mask, IPI_CALL_FUNC);
}

void arch_send_call_function_single_ipi(int cpu)
{
	smp_cross_call_common(cpumask_of(cpu), IPI_CALL_FUNC);
}

void arch_send_wakeup_ipi_mask(const struct cpumask *mask)
{
	smp_cross_call_common(mask, IPI_WAKEUP);
}

#ifdef CONFIG_IRQ_WORK
void arch_irq_work_raise(void)
{
	if (__smp_cross_call)
		smp_cross_call_common(cpumask_of(smp_processor_id()),
				      IPI_IRQ_WORK);
}
#endif

static DEFINE_RAW_SPINLOCK(stop_lock);

DEFINE_PER_CPU(struct pt_regs, regs_before_stop);

/*
 * ipi_cpu_stop - handle IPI from smp_send_stop()
 */
static void ipi_cpu_stop(unsigned int cpu, struct pt_regs *regs)
{
	if (system_state == SYSTEM_BOOTING ||
	    system_state == SYSTEM_RUNNING) {
		per_cpu(regs_before_stop, cpu) = *regs;
		raw_spin_lock(&stop_lock);
		pr_crit("CPU%u: stopping\n", cpu);
		show_regs(regs);
		dump_stack();
		dump_stack_minidump(regs->sp);
		raw_spin_unlock(&stop_lock);
	}

	set_cpu_active(cpu, false);

	flush_cache_all();
	local_irq_disable();

	while (1)
		cpu_relax();
}

static cpumask_t backtrace_mask;
static DEFINE_RAW_SPINLOCK(backtrace_lock);

/* "in progress" flag of arch_trigger_all_cpu_backtrace */
static unsigned long backtrace_flag;

static void smp_send_all_cpu_backtrace(void)
{
	unsigned int this_cpu = smp_processor_id();
	int i;

	if (test_and_set_bit(0, &backtrace_flag))
		/*
		 * If there is already a trigger_all_cpu_backtrace() in progress
		 * (backtrace_flag == 1), don't output double cpu dump infos.
		 */
		return;

	cpumask_copy(&backtrace_mask, cpu_online_mask);
	cpumask_clear_cpu(this_cpu, &backtrace_mask);

	pr_info("Backtrace for cpu %d (current):\n", this_cpu);
	dump_stack();

	pr_info("\nsending IPI to all other CPUs:\n");
	if (!cpumask_empty(&backtrace_mask))
		smp_cross_call(&backtrace_mask, IPI_CPU_BACKTRACE);

	/* Wait for up to 10 seconds for all other CPUs to do the backtrace */
	for (i = 0; i < 10 * 1000; i++) {
		if (cpumask_empty(&backtrace_mask))
			break;
		mdelay(1);
	}

	clear_bit(0, &backtrace_flag);
	smp_mb__after_atomic();
}

/*
 * ipi_cpu_backtrace - handle IPI from smp_send_all_cpu_backtrace()
 */
static void ipi_cpu_backtrace(unsigned int cpu, struct pt_regs *regs)
{
	if (cpumask_test_cpu(cpu, &backtrace_mask)) {
		raw_spin_lock(&backtrace_lock);
		pr_warn("IPI backtrace for cpu %d\n", cpu);
		show_regs(regs);
		raw_spin_unlock(&backtrace_lock);
		cpumask_clear_cpu(cpu, &backtrace_mask);
	}
}

#ifdef CONFIG_SMP
void arch_trigger_all_cpu_backtrace(void)
{
	smp_send_all_cpu_backtrace();
}
#else
void arch_trigger_all_cpu_backtrace(void)
{
	dump_stack();
}
#endif


/*
 * Main handler for inter-processor interrupts
 */
void handle_IPI(int ipinr, struct pt_regs *regs)
{
	unsigned int cpu = smp_processor_id();
	struct pt_regs *old_regs = set_irq_regs(regs);

	if ((unsigned)ipinr < NR_IPI) {
		trace_ipi_entry_rcuidle(ipi_types[ipinr]);
		__inc_irq_stat(cpu, ipi_irqs[ipinr]);
	}

	switch (ipinr) {
	case IPI_RESCHEDULE:
		scheduler_ipi();
		break;

	case IPI_CALL_FUNC:
		irq_enter();
		generic_smp_call_function_interrupt();
		irq_exit();
		break;

	case IPI_CPU_STOP:
		irq_enter();
		ipi_cpu_stop(cpu, regs);
		irq_exit();
		break;

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
	case IPI_TIMER:
		irq_enter();
		tick_receive_broadcast();
		irq_exit();
		break;
#endif

#ifdef CONFIG_IRQ_WORK
	case IPI_IRQ_WORK:
		irq_enter();
		irq_work_run();
		irq_exit();
		break;
#endif

	case IPI_WAKEUP:
		break;

	case IPI_CPU_BACKTRACE:
		ipi_cpu_backtrace(cpu, regs);
		break;

	default:
		pr_crit("CPU%u: Unknown IPI message 0x%x\n", cpu, ipinr);
		break;
	}

	if ((unsigned)ipinr < NR_IPI)
		trace_ipi_exit_rcuidle(ipi_types[ipinr]);
	per_cpu(pending_ipi, cpu) = false;
	set_irq_regs(old_regs);
}

void smp_send_reschedule(int cpu)
{
	BUG_ON(cpu_is_offline(cpu));
	update_ipi_history(cpu);
	smp_cross_call_common(cpumask_of(cpu), IPI_RESCHEDULE);
}

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
void tick_broadcast(const struct cpumask *mask)
{
	smp_cross_call_common(mask, IPI_TIMER);
}
#endif

/*
 * The number of CPUs online, not counting this CPU (which may not be
 * fully online and so not counted in num_online_cpus()).
 */
static inline unsigned int num_other_online_cpus(void)
{
	unsigned int this_cpu_online = cpu_online(smp_processor_id());

	return num_online_cpus() - this_cpu_online;
}

static inline unsigned int num_other_active_cpus(void)
{
	unsigned int this_cpu_active = cpu_active(smp_processor_id());

	return num_active_cpus() - this_cpu_active;
}

void smp_send_stop(void)
{
	unsigned long timeout;

	if (num_other_online_cpus()) {
		cpumask_t mask;

		cpumask_copy(&mask, cpu_online_mask);
		cpumask_clear_cpu(smp_processor_id(), &mask);

		if (system_state == SYSTEM_BOOTING ||
		    system_state == SYSTEM_RUNNING)
			pr_crit("SMP: stopping secondary CPUs\n");
		smp_cross_call_common(&mask, IPI_CPU_STOP);
	}

	/* Wait up to one second for other CPUs to stop */
	timeout = USEC_PER_SEC;
	while (num_other_active_cpus() && timeout--)
		udelay(1);

	if (num_other_active_cpus())
		pr_warning("SMP: failed to stop secondary CPUs %*pbl\n",
			   cpumask_pr_args(cpu_online_mask));
}

/*
 * not supported here
 */
int setup_profiling_timer(unsigned int multiplier)
{
	return -EINVAL;
}

static bool have_cpu_die(void)
{
#ifdef CONFIG_HOTPLUG_CPU
	int any_cpu = raw_smp_processor_id();

	if (cpu_ops[any_cpu] && cpu_ops[any_cpu]->cpu_die)
		return true;
#endif
	return false;
}

bool cpus_are_stuck_in_kernel(void)
{
	bool smp_spin_tables = (num_possible_cpus() > 1 && !have_cpu_die());

	return !!cpus_stuck_in_kernel || smp_spin_tables;
}
