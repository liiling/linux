// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel-based Virtual Machine driver for Linux
 *
 * Copyright 2016 Red Hat, Inc. and/or its affiliates.
 */
#include <linux/kvm_host.h>
#include <linux/stats_fs.h>
#include "lapic.h"

#define VCPU_ARCH_STATS_FS(n, s, x, ...)					\
			{ n, offsetof(struct s, x), .aggr_kind = STATS_FS_SUM,	\
			  ##__VA_ARGS__ }

struct stats_fs_value stats_fs_vcpu_tsc_offset[] = {
	VCPU_ARCH_STATS_FS("tsc-offset", kvm_vcpu_arch, tsc_offset,
			   .type = STATS_FS_S64, .mode = 0444),
	{ NULL }
};

struct stats_fs_value stats_fs_vcpu_arch_lapic_timer[] = {
	VCPU_ARCH_STATS_FS("lapic_timer_advance_ns", kvm_timer, timer_advance_ns,
			   .type = STATS_FS_U64, .mode = 0444),
	{ NULL }
};

struct stats_fs_value stats_fs_vcpu_arch_tsc_ratio[] = {
	VCPU_ARCH_STATS_FS("tsc-scaling-ratio", kvm_vcpu_arch, tsc_scaling_ratio,
			   .type = STATS_FS_U64, .mode = 0444),
	{ NULL }
};

struct stats_fs_value stats_fs_vcpu_arch_tsc_frac[] = {
	{ "tsc-scaling-ratio-frac-bits", 0, .type = STATS_FS_U64, .mode = 0444 },
	{ NULL } /* base is &kvm_tsc_scaling_ratio_frac_bits */
};

void kvm_arch_create_vcpu_stats_fs(struct kvm_vcpu *vcpu)
{
	stats_fs_source_add_values(vcpu->stats_fs_src, stats_fs_vcpu_tsc_offset,
				   &vcpu->arch);

	if (lapic_in_kernel(vcpu))
		stats_fs_source_add_values(vcpu->stats_fs_src,
					   stats_fs_vcpu_arch_lapic_timer,
					   &vcpu->arch.apic->lapic_timer);

	if (kvm_has_tsc_control) {
		stats_fs_source_add_values(vcpu->stats_fs_src,
					   stats_fs_vcpu_arch_tsc_ratio,
					   &vcpu->arch);
		stats_fs_source_add_values(vcpu->stats_fs_src,
					   stats_fs_vcpu_arch_tsc_frac,
					   &kvm_tsc_scaling_ratio_frac_bits);
	}
}
