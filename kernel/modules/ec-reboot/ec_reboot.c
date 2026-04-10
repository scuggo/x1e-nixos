// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/surface_aggregator/controller.h>

static ssize_t reboot_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct ssam_controller *ctrl;
	struct ssam_request rqst = {};
	int ret;

	if (count < 1 || buf[0] != '1')
		return -EINVAL;

	ctrl = ssam_get_controller();
	if (!ctrl)
		return -ENODEV;

	rqst.target_category = 0x01;
	rqst.target_id = 0x01;
	rqst.command_id = 0x14;
	rqst.instance_id = 0x00;
	rqst.flags = 0;
	rqst.length = 0;
	rqst.payload = NULL;

	ret = ssam_request_do_sync(ctrl, &rqst, NULL);
	if (ret)
		return ret;

	return count;
}

static struct kobj_attribute reboot_attr = __ATTR_WO(reboot);

static struct attribute *ec_reboot_attrs[] = {
	&reboot_attr.attr,
	NULL,
};

static const struct attribute_group ec_reboot_group = {
	.attrs = ec_reboot_attrs,
};

static struct kobject *ec_reboot_kobj;

static int __init ec_reboot_init(void)
{
	int ret;

	ec_reboot_kobj = kobject_create_and_add("ec_reboot", kernel_kobj);
	if (!ec_reboot_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(ec_reboot_kobj, &ec_reboot_group);
	if (ret)
		kobject_put(ec_reboot_kobj);

	return ret;
}

static void __exit ec_reboot_exit(void)
{
	kobject_put(ec_reboot_kobj);
}

module_init(ec_reboot_init);
module_exit(ec_reboot_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("EC hard reset via SSAM");
