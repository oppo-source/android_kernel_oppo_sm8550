// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.*/

#include <linux/io.h>
#include <linux/kmsg_dump.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/proc_fs.h>
#include <linux/skbuff.h>
#include <linux/suspend.h>
#include <linux/types.h>
#include <linux/gunyah/gh_dbl.h>
#include <linux/gunyah/gh_rm_drv.h>
#include <soc/qcom/secure_buffer.h>

#include "dmesg_dumper_private.h"

#define DDUMP_DBL_MASK				0x1
#define DDUMP_PROFS_NAME			"vmkmsg"
#define DDUMP_WAIT_WAKEIRQ_TIMEOUT	msecs_to_jiffies(1000)

static void qcom_ddump_to_shm(struct kmsg_dumper *dumper,
			  enum kmsg_dump_reason reason)
{
	struct qcom_dmesg_dumper *qdd = container_of(dumper,
					struct qcom_dmesg_dumper, dump);
	size_t len;

	dev_warn(qdd->dev, "reason = %d\n", reason);
	kmsg_dump_rewind(&qdd->iter);
	memset(qdd->base, 0, qdd->size);
	kmsg_dump_get_buffer(&qdd->iter, true, qdd->base, qdd->size, &len);
	dev_warn(qdd->dev, "size of dmesg logbuf logged = %lld\n", len);
}

static struct device_node *qcom_ddump_svm_of_parse(struct qcom_dmesg_dumper *qdd)
{
	const char *compat = "qcom,ddump-gunyah-gen";
	struct device_node *np = NULL;
	struct device_node *shm_np;
	u32 label;
	int ret;

	while ((np = of_find_compatible_node(np, NULL, compat))) {
		ret = of_property_read_u32(np, "qcom,label", &label);
		if (ret) {
			of_node_put(np);
			continue;
		}
		if (label == qdd->label)
			break;

		of_node_put(np);
	}
	if (!np)
		return NULL;

	shm_np = of_parse_phandle(np, "memory-region", 0);
	of_node_put(np);

	return shm_np;
}

static int qcom_ddump_map_memory(struct qcom_dmesg_dumper *qdd)
{
	struct device *dev = qdd->dev;
	struct device_node *np;
	int ret;

	np = of_parse_phandle(dev->of_node, "shared-buffer", 0);
	if (!np) {
		/*
		 * "shared-buffer" is only specified for primary VM.
		 * Parse "memory-region" for the hypervisor-generated node for
		 * secondary VM.
		 */
		np = qcom_ddump_svm_of_parse(qdd);
		if (!np) {
			dev_err(dev, "Unable to parse shared mem node\n");
			return -EINVAL;
		}
	}

	ret = of_address_to_resource(np, 0, &qdd->res);
	of_node_put(np);
	if (ret) {
		dev_err(dev, "of_address_to_resource failed!\n");
		return -EINVAL;
	}
	qdd->size = resource_size(&qdd->res);

	return 0;
}

static int qcom_ddump_share_mem(struct qcom_dmesg_dumper *qdd, gh_vmid_t self,
			   gh_vmid_t peer)
{
	u32 src_vmlist[1] = {self};
	int src_perms[2] = {PERM_READ | PERM_WRITE | PERM_EXEC};
	int dst_vmlist[2] = {self, peer};
	int dst_perms[2] = {PERM_READ | PERM_WRITE, PERM_READ | PERM_WRITE};
	struct gh_acl_desc *acl;
	struct gh_sgl_desc *sgl;
	int ret;

	ret = hyp_assign_phys(qdd->res.start, resource_size(&qdd->res),
			      src_vmlist, 1,
			      dst_vmlist, dst_perms, 2);
	if (ret) {
		dev_err(qdd->dev, "hyp_assign_phys addr=%x size=%u failed: %d\n",
		       qdd->res.start, qdd->size, ret);
		return ret;
	}

	acl = kzalloc(offsetof(struct gh_acl_desc, acl_entries[2]), GFP_KERNEL);
	if (!acl)
		return -ENOMEM;
	sgl = kzalloc(offsetof(struct gh_sgl_desc, sgl_entries[1]), GFP_KERNEL);
	if (!sgl) {
		kfree(acl);
		return -ENOMEM;
	}
	acl->n_acl_entries = 2;
	acl->acl_entries[0].vmid = (u16)self;
	acl->acl_entries[0].perms = GH_RM_ACL_R | GH_RM_ACL_W;
	acl->acl_entries[1].vmid = (u16)peer;
	acl->acl_entries[1].perms = GH_RM_ACL_R | GH_RM_ACL_W;

	sgl->n_sgl_entries = 1;
	sgl->sgl_entries[0].ipa_base = qdd->res.start;
	sgl->sgl_entries[0].size = resource_size(&qdd->res);

	ret = gh_rm_mem_share(GH_RM_MEM_TYPE_NORMAL, 0, qdd->label,
			      acl, sgl, NULL, &qdd->memparcel);
	if (ret) {
		dev_err(qdd->dev, "Gunyah mem share addr=%x size=%u failed: %d\n",
		       qdd->res.start, qdd->size, ret);
		/* Attempt to give resource back to HLOS */
		hyp_assign_phys(qdd->res.start, resource_size(&qdd->res),
				dst_vmlist, 2,
				src_vmlist, src_perms, 1);
	}

	kfree(acl);
	kfree(sgl);

	return ret;
}

static void qcom_ddump_unshare_mem(struct qcom_dmesg_dumper *qdd, gh_vmid_t self,
			      gh_vmid_t peer)
{
	int dst_perms[2] = {PERM_READ | PERM_WRITE | PERM_EXEC};
	int src_vmlist[2] = {self, peer};
	u32 dst_vmlist[1] = {self};
	int ret;

	ret = gh_rm_mem_reclaim(qdd->memparcel, 0);
	if (ret)
		dev_err(qdd->dev, "Gunyah mem reclaim failed: %d\n", ret);

	hyp_assign_phys(qdd->res.start, resource_size(&qdd->res),
			src_vmlist, 2, dst_vmlist, dst_perms, 1);
}

static int qcom_ddump_rm_cb(struct notifier_block *nb, unsigned long cmd,
			     void *data)
{
	struct gh_rm_notif_vm_status_payload *vm_status_payload;
	struct qcom_dmesg_dumper *qdd;
	gh_vmid_t peer_vmid;
	gh_vmid_t self_vmid;

	qdd = container_of(nb, struct qcom_dmesg_dumper, rm_nb);

	if (cmd != GH_RM_NOTIF_VM_STATUS)
		return NOTIFY_DONE;

	vm_status_payload = data;
	if (vm_status_payload->vm_status != GH_RM_VM_STATUS_READY &&
	    vm_status_payload->vm_status != GH_RM_VM_STATUS_RESET)
		return NOTIFY_DONE;
	if (gh_rm_get_vmid(qdd->peer_name, &peer_vmid))
		return NOTIFY_DONE;
	if (gh_rm_get_vmid(GH_PRIMARY_VM, &self_vmid))
		return NOTIFY_DONE;
	if (peer_vmid != vm_status_payload->vmid)
		return NOTIFY_DONE;

	if (vm_status_payload->vm_status == GH_RM_VM_STATUS_READY) {
		if (qcom_ddump_share_mem(qdd, self_vmid, peer_vmid)) {
			dev_err(qdd->dev, "Failed to share memory\n");
			return NOTIFY_DONE;
		}
	}

	if (vm_status_payload->vm_status == GH_RM_VM_STATUS_RESET)
		qcom_ddump_unshare_mem(qdd, self_vmid, peer_vmid);

	return NOTIFY_DONE;
}

static inline int qcom_ddump_gh_kick(struct qcom_dmesg_dumper *qdd)
{
	gh_dbl_flags_t dbl_mask = DDUMP_DBL_MASK;
	int ret;

	ret = gh_dbl_send(qdd->tx_dbl, &dbl_mask, 0);
	if (ret)
		dev_err(qdd->dev, "failed to raise virq to the sender %d\n", ret);

	return ret;
}

static void qcom_ddump_gh_cb(int irq, void *data)
{
	gh_dbl_flags_t dbl_mask = DDUMP_DBL_MASK;
	struct qcom_dmesg_dumper *qdd;
	struct ddump_shm_hdr *hdr;
	int ret;

	qdd = data;
	hdr = qdd->base;
	gh_dbl_read_and_clean(qdd->rx_dbl, &dbl_mask, GH_DBL_NONBLOCK);

	if (qdd->primary_vm) {
		complete(&qdd->ddump_completion);
	} else {
		/* avoid system enter suspend */
		pm_wakeup_ws_event(qdd->wakeup_source, 2000, true);
		ret = qcom_ddump_alive_log_to_shm(qdd, hdr->user_buf_len);
		if (ret)
			dev_err(qdd->dev, "dump alive log error %d\n", ret);

		qcom_ddump_gh_kick(qdd);
		if (hdr->svm_dump_len == 0)
			pm_wakeup_ws_event(qdd->wakeup_source, 0, true);
	}
}

static ssize_t qcom_ddump_vmkmsg_read(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct qcom_dmesg_dumper *qdd = PDE_DATA(file_inode(file));
	struct ddump_shm_hdr *hdr = qdd->base;
	int ret;

	if (count < LOG_LINE_MAX) {
		dev_err(qdd->dev, "user buffer size should greater than %d\n", LOG_LINE_MAX);
		return -EINVAL;
	}

	/**
	 * If SVM is in suspend mode and the log size more than 1k byte,
	 * we think SVM has log need to be read. Otherwise, we think the
	 * log is only suspend log that we need skip the unnecessary log.
	 */
	if (hdr->svm_is_suspend && hdr->svm_dump_len < 1024)
		return 0;

	hdr->user_buf_len = count;
	qcom_ddump_gh_kick(qdd);
	ret = wait_for_completion_timeout(&qdd->ddump_completion, DDUMP_WAIT_WAKEIRQ_TIMEOUT);
	if (!ret) {
		dev_err(qdd->dev, "wait for completion timeout\n");
		return -ETIMEDOUT;
	}

	if (hdr->svm_dump_len > count) {
		dev_err(qdd->dev, "can not read the correct length of svm kmsg\n");
		return -EINVAL;
	}

	if (hdr->svm_dump_len &&
		copy_to_user(buf, &hdr->data, hdr->svm_dump_len)) {
		dev_err(qdd->dev, "copy_to_user fail\n");
		return -EFAULT;
	}

	return hdr->svm_dump_len;
}

static const struct proc_ops ddump_proc_ops = {
	.proc_flags	= PROC_ENTRY_PERMANENT,
	.proc_read	= qcom_ddump_vmkmsg_read,
};

static int qcom_ddump_alive_log_probe(struct qcom_dmesg_dumper *qdd)
{
	struct device_node *node = qdd->dev->of_node;
	struct device *dev = qdd->dev;
	struct proc_dir_entry *dent;
	struct ddump_shm_hdr *hdr;
	enum gh_dbl_label dbl_label;
	struct resource *res;
	size_t shm_min_size;
	int ret;

	shm_min_size = LOG_LINE_MAX + DDUMP_GET_SHM_HDR;
	if (qdd->size < shm_min_size) {
		dev_err(dev, "Shared memory size should greater than %d\n", shm_min_size);
		return -EINVAL;
	}

	dbl_label = qdd->label;
	qdd->tx_dbl = gh_dbl_tx_register(dbl_label);
	if (IS_ERR_OR_NULL(qdd->tx_dbl)) {
		ret = PTR_ERR(qdd->tx_dbl);
		dev_err(dev, "%s:Failed to get gunyah tx dbl %d\n", __func__, ret);
		return ret;
	}

	qdd->rx_dbl = gh_dbl_rx_register(dbl_label, qcom_ddump_gh_cb, qdd);
	if (IS_ERR_OR_NULL(qdd->rx_dbl)) {
		ret = PTR_ERR(qdd->rx_dbl);
		dev_err(dev, "%s:Failed to get gunyah rx dbl %d\n", __func__, ret);
		goto err_unregister_tx_dbl;
	}

	if (qdd->primary_vm) {
		res = devm_request_mem_region(dev, qdd->res.start, qdd->size, dev_name(dev));
		if (!res) {
			ret = -ENXIO;
			dev_err(dev, "request mem region fail\n");
			goto err_unregister_rx_dbl;
		}

		qdd->base = devm_ioremap_wc(dev, qdd->res.start, qdd->size);
		if (!qdd->base) {
			ret = -ENOMEM;
			dev_err(dev, "devm_ioremap_wc fail\n");
			goto err_unregister_rx_dbl;
		}

		init_completion(&qdd->ddump_completion);
		dent = proc_create_data(DDUMP_PROFS_NAME, 0400, NULL, &ddump_proc_ops, qdd);
		if (!dent) {
			dev_err(dev, "proc_create_data fail\n");
			ret = -ENOMEM;
			goto err_unregister_rx_dbl;
		}
	} else {
		qdd->wakeup_source = wakeup_source_register(dev, dev_name(dev));
		if (!qdd->wakeup_source) {
			ret = -ENOMEM;
			goto err_unregister_rx_dbl;
		}

		/* init shared memory header */
		hdr = qdd->base;
		hdr->svm_is_suspend = false;

		ret = qcom_ddump_encrypt_init(node);
		if (ret)
			goto err_unregister_wakeup_source;
	}

	return 0;
err_unregister_wakeup_source:
	wakeup_source_unregister(qdd->wakeup_source);
err_unregister_rx_dbl:
	gh_dbl_rx_unregister(qdd->rx_dbl);
err_unregister_tx_dbl:
	gh_dbl_tx_unregister(qdd->tx_dbl);

	return ret;
}

static int qcom_ddump_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct qcom_dmesg_dumper *qdd;
	struct device *dev;
	int ret;
	struct resource *res;

	qdd = devm_kzalloc(&pdev->dev, sizeof(*qdd), GFP_KERNEL);
	if (!qdd)
		return -ENOMEM;

	qdd->dev = &pdev->dev;
	platform_set_drvdata(pdev, qdd);

	dev = qdd->dev;
	ret = of_property_read_u32(node, "gunyah-label", &qdd->label);
	if (ret) {
		dev_err(dev, "Failed to read label %d\n", ret);
		return ret;
	}

	qdd->primary_vm = of_property_read_bool(node, "qcom,primary-vm");

	ret = qcom_ddump_map_memory(qdd);
	if (ret)
		return ret;

	if (qdd->primary_vm) {
		ret = of_property_read_u32(node, "peer-name", &qdd->peer_name);
		if (ret)
			qdd->peer_name = GH_SELF_VM;

		qdd->rm_nb.notifier_call = qcom_ddump_rm_cb;
		qdd->rm_nb.priority = INT_MAX;
		gh_rm_register_notifier(&qdd->rm_nb);
	} else {
		res = devm_request_mem_region(dev, qdd->res.start, qdd->size, dev_name(dev));
		if (!res) {
			dev_err(dev, "request mem region fail\n");
			return -ENXIO;
		}

		qdd->base = devm_ioremap_wc(dev, qdd->res.start, qdd->size);
		if (!qdd->base) {
			dev_err(dev, "ioremap fail\n");
			return -ENOMEM;
		}

		kmsg_dump_rewind(&qdd->iter);
		qdd->dump.dump = qcom_ddump_to_shm;
		ret = kmsg_dump_register(&qdd->dump);
		if (ret)
			return ret;
	}

	if (IS_ENABLED(CONFIG_QCOM_VM_ALIVE_LOG_DUMPER)) {
		ret = qcom_ddump_alive_log_probe(qdd);
		if (ret) {
			if (qdd->primary_vm)
				gh_rm_unregister_notifier(&qdd->rm_nb);
			else
				kmsg_dump_unregister(&qdd->dump);
			return ret;
		}
	}

	return 0;
}

static int qcom_ddump_remove(struct platform_device *pdev)
{
	int ret;
	struct qcom_dmesg_dumper *qdd = platform_get_drvdata(pdev);

	if (IS_ENABLED(CONFIG_QCOM_VM_ALIVE_LOG_DUMPER)) {
		gh_dbl_tx_unregister(qdd->tx_dbl);
		gh_dbl_rx_unregister(qdd->rx_dbl);
		if (qdd->primary_vm) {
			remove_proc_entry(DDUMP_PROFS_NAME, NULL);
		} else {
			wakeup_source_unregister(qdd->wakeup_source);
			qcom_ddump_encrypt_exit();
		}
	}

	if (qdd->primary_vm) {
		gh_rm_unregister_notifier(&qdd->rm_nb);
	} else {
		ret = kmsg_dump_unregister(&qdd->dump);
		if (ret)
			return ret;
	}

	return 0;
}

#if IS_ENABLED(CONFIG_PM_SLEEP) && IS_ENABLED(CONFIG_ARCH_QTI_VM) && \
	IS_ENABLED(CONFIG_QCOM_VM_ALIVE_LOG_DUMPER)
static int qcom_ddump_suspend(struct device *pdev)
{
	struct qcom_dmesg_dumper *qdd = dev_get_drvdata(pdev);
	struct ddump_shm_hdr *hdr = qdd->base;
	u64 seq_backup;
	int ret;

	hdr->svm_is_suspend = true;
	seq_backup = qdd->iter.cur_seq;
	ret = qcom_ddump_alive_log_to_shm(qdd, qdd->size);
	if (ret)
		dev_err(qdd->dev, "dump alive log error %d\n", ret);

	qdd->iter.cur_seq = seq_backup;
	return 0;
}

static int qcom_ddump_resume(struct device *pdev)
{
	struct qcom_dmesg_dumper *qdd = dev_get_drvdata(pdev);
	struct ddump_shm_hdr *hdr = qdd->base;

	hdr->svm_is_suspend = false;
	return 0;
}

static SIMPLE_DEV_PM_OPS(ddump_pm_ops, qcom_ddump_suspend, qcom_ddump_resume);
#endif

static const struct of_device_id ddump_match_table[] = {
	{ .compatible = "qcom,dmesg-dump" },
	{}
};

static struct platform_driver ddump_driver = {
	.driver = {
		.name = "qcom_dmesg_dumper",
#if IS_ENABLED(CONFIG_PM_SLEEP) && IS_ENABLED(CONFIG_ARCH_QTI_VM) && \
	IS_ENABLED(CONFIG_QCOM_VM_ALIVE_LOG_DUMPER)
		.pm = &ddump_pm_ops,
#endif
		.of_match_table = ddump_match_table,
	 },
	.probe = qcom_ddump_probe,
	.remove = qcom_ddump_remove,
};

static int __init qcom_ddump_init(void)
{
	return platform_driver_register(&ddump_driver);
}

#if IS_ENABLED(CONFIG_ARCH_QTI_VM)
arch_initcall(qcom_ddump_init);
#else
module_init(qcom_ddump_init);
#endif

static __exit void qcom_ddump_exit(void)
{
	platform_driver_unregister(&ddump_driver);
}
module_exit(qcom_ddump_exit);

MODULE_DESCRIPTION("QTI Virtual Machine dmesg log buffer dumper");
MODULE_LICENSE("GPL v2");
