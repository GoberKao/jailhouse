/*
 * Jailhouse AArch64 support
 *
 * Copyright (C) 2015 Huawei Technologies Duesseldorf GmbH
 *
 * Authors:
 *  Antonios Motakis <antonios.motakis@huawei.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/cell.h>
#include <jailhouse/entry.h>
#include <jailhouse/paging.h>
#include <jailhouse/printk.h>
#include <jailhouse/processor.h>
#include <asm/control.h>
#include <asm/entry.h>
#include <asm/irqchip.h>
#include <asm/setup.h>

extern u8 __trampoline_start[];

int arch_init_early(void)
{
	unsigned long trampoline_page = paging_hvirt2phys(&__trampoline_start);
	int err;

	/*
	 * ID-map the trampoline code page.
	 *
	 * We will need it for shutting down once the final page table is
	 * installed. So better do this early while we can still handle errors.
	 */
	err = paging_create(&hv_paging_structs, trampoline_page, PAGE_SIZE,
			    trampoline_page, PAGE_DEFAULT_FLAGS,
			    PAGING_NON_COHERENT);
	if (err)
		return err;

	return arm_init_early();
}

int arch_cpu_init(struct per_cpu *cpu_data)
{
	unsigned long hcr = HCR_VM_BIT | HCR_IMO_BIT | HCR_FMO_BIT
				| HCR_TSC_BIT | HCR_TAC_BIT | HCR_RW_BIT;

	/* switch to the permanent page tables */
	enable_mmu_el2(paging_hvirt2phys(hv_paging_structs.root_table));

	/* Setup guest traps */
	arm_write_sysreg(HCR_EL2, hcr);

	return arm_cpu_init(cpu_data);
}

int arch_init_late(void)
{
	return arm_init_late();
}

void __attribute__((noreturn)) arch_cpu_activate_vmm(struct per_cpu *cpu_data)
{
	struct registers *regs = guest_regs(cpu_data);

	/* return to the caller in Linux */
	arm_write_sysreg(ELR_EL2, regs->usr[30]);

	vmreturn(regs);
}

/* disable the hypervisor on the current CPU */
void arch_shutdown_self(struct per_cpu *cpu_data)
{
	void (*shutdown_func)(struct per_cpu *) =
		(void (*)(struct per_cpu *))paging_hvirt2phys(shutdown_el2);

	irqchip_cpu_shutdown(cpu_data);

	/* Free the guest */
	arm_write_sysreg(HCR_EL2, HCR_RW_BIT);
	arm_write_sysreg(VTCR_EL2, VTCR_RES1);

	/* Remove stage-2 mappings */
	arm_paging_vcpu_flush_tlbs();

	/* TLB flush needs the cell's VMID */
	isb();
	arm_write_sysreg(VTTBR_EL2, 0);

	/* we will restore the root cell state with the MMU turned off,
	 * so we need to make sure it has been committed to memory */
	arch_paging_flush_cpu_caches(cpu_data, sizeof(*cpu_data));
	dsb(ish);

	/* hand over control of EL2 back to Linux */
	asm volatile("msr vbar_el2, %0"
		:: "r" (hypervisor_header.arm_linux_hyp_vectors));

	/* Return to EL1 */
	shutdown_func((struct per_cpu *)paging_hvirt2phys(cpu_data));
}

void arch_cpu_restore(struct per_cpu *cpu_data, int return_code)
{
	struct registers *regs = guest_regs(cpu_data);

	/* Jailhouse initialization failed; return to the caller in EL1 */
	arm_write_sysreg(ELR_EL2, regs->usr[30]);

	regs->usr[0] = return_code;

	arch_shutdown_self(cpu_data);
}
