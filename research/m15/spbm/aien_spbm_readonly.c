// SPDX-License-Identifier: GPL-2.0-only
/* Machine-pinned GB10 SPBM measurement reader.
 * The ACPI discovery protocol and milli-unit convention were documented by
 * antheas/spark_hwmon (GPL-2.0). This reader checks the local firmware's
 * contract, exclusively claims its resource, and exposes no writes.
 * It never accesses the firmware's CLEAR_OVERFLOW registers.
 */
#include <linux/acpi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "contract.h"

struct aien_spbm { void __iomem *base; };
static const char * const energy_labels[] = {
	"pkg", "cpu_e", "cpu_p", "gpc_unverified", "gpm"
};
static const char * const power_labels[] = {
	"sys_total", "soc_pkg", "cpu_e", "cpu_p", "gpu"
};
static const guid_t mtel_guid = GUID_INIT(0x12345678, 0x1234, 0x1234,
	0x12, 0x34, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc);

static int spbm_resource_index(acpi_handle handle)
{
	union acpi_object *names;
	unsigned int i;
	int index = -ENODEV;
	names = acpi_evaluate_dsm(handle, &mtel_guid, 0, 1, NULL);
	if (!names)
		return -ENODEV;
	if (names->type != ACPI_TYPE_PACKAGE)
		goto out;
	for (i = 0; i < names->package.count; ++i) {
		union acpi_object *name = &names->package.elements[i];
		if (name->type != ACPI_TYPE_STRING || name->string.length != 4 ||
		    memcmp(name->string.pointer, "SPBM", 4))
			continue;
		if (index >= 0) {
			index = -EINVAL;
			break;
		}
		index = i;
	}
out:
	ACPI_FREE(names);
	return index;
}

static int spbm_check_registers(acpi_handle handle, unsigned int index)
{
	union acpi_object arg = { .type = ACPI_TYPE_INTEGER };
	union acpi_object args = { .type = ACPI_TYPE_PACKAGE };
	union acpi_object *map;
	unsigned long seen = 0;
	unsigned int i, j;
	int ret = -EINVAL;
	arg.integer.value = index;
	args.package.count = 1;
	args.package.elements = &arg;
	map = acpi_evaluate_dsm(handle, &mtel_guid, 0, 2, &args);
	if (!map)
		return -ENODEV;
	if (map->type != ACPI_TYPE_PACKAGE)
		goto out;
	for (i = 0; i < map->package.count; ++i) {
		union acpi_object *group = &map->package.elements[i];
		union acpi_object *entry;
		if (group->type != ACPI_TYPE_PACKAGE || !group->package.count)
			goto out;
		entry = group->package.elements;
		if (entry[0].type != ACPI_TYPE_INTEGER ||
		    (group->package.count - 1) % 2 ||
		    entry[0].integer.value != (group->package.count - 1) / 2)
			goto out;
		for (j = 1; j < group->package.count; j += 2) {
			if (entry[j].type != ACPI_TYPE_STRING ||
			    entry[j + 1].type != ACPI_TYPE_INTEGER ||
			    spbm_contract_add(&seen, entry[j].string.pointer,
				entry[j].string.length, entry[j + 1].integer.value) < 0)
				goto out;
		}
	}
	if (seen == SPBM_ALL_REGS)
		ret = 0;
out:
	ACPI_FREE(map);
	return ret;
}

static u32 spbm_read_reg(struct aien_spbm *spbm, unsigned int index)
{
	return readl(spbm->base + spbm_registers[index].offset);
}

static umode_t aien_spbm_visible(const void *data,
			enum hwmon_sensor_types type, u32 attr, int channel)
{
	if (channel < 0)
		return 0;
	if (type == hwmon_energy && channel < SPBM_N_ENERGY &&
	    (attr == hwmon_energy_input || attr == hwmon_energy_label))
		return 0444;
	if (type == hwmon_power && channel < SPBM_N_POWER &&
	    (attr == hwmon_power_input || attr == hwmon_power_label))
		return 0444;
	return 0;
}

static int aien_spbm_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *value)
{
	struct aien_spbm *spbm = dev_get_drvdata(dev);
	u32 raw;
	if (channel < 0)
		return -EOPNOTSUPP;
	if (type == hwmon_energy && attr == hwmon_energy_input &&
	    channel < SPBM_N_ENERGY) {
		/* Overflow semantics are undocumented. Never silently unwrap it. */
		if (spbm_read_reg(spbm, SPBM_OVERFLOW_FIRST + channel))
			return -EOVERFLOW;
		raw = spbm_read_reg(spbm, channel);
		if (spbm_read_reg(spbm, SPBM_OVERFLOW_FIRST + channel))
			return -EOVERFLOW;
	} else if (type == hwmon_power && attr == hwmon_power_input &&
		   channel < SPBM_N_POWER) {
		raw = spbm_read_reg(spbm, SPBM_POWER_FIRST + channel);
	} else {
		return -EOPNOTSUPP;
	}
	*value = (long)raw * 1000L;
	return 0;
}

static int aien_spbm_read_string(struct device *dev,
			 enum hwmon_sensor_types type, u32 attr,
			 int channel, const char **value)
{
	if (channel < 0)
		return -EOPNOTSUPP;
	if (type == hwmon_energy && attr == hwmon_energy_label &&
	    channel < SPBM_N_ENERGY)
		*value = energy_labels[channel];
	else if (type == hwmon_power && attr == hwmon_power_label &&
		 channel < SPBM_N_POWER)
		*value = power_labels[channel];
	else
		return -EOPNOTSUPP;
	return 0;
}

static ssize_t overflow_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct aien_spbm *spbm = dev_get_drvdata(dev);
	unsigned int index = to_sensor_dev_attr(attr)->index;
	return sysfs_emit(buf, "%u\n", spbm_read_reg(spbm, SPBM_OVERFLOW_FIRST + index));
}
static SENSOR_DEVICE_ATTR_RO(energy1_overflow_raw, overflow, 0);
static SENSOR_DEVICE_ATTR_RO(energy2_overflow_raw, overflow, 1);
static SENSOR_DEVICE_ATTR_RO(energy3_overflow_raw, overflow, 2);
static SENSOR_DEVICE_ATTR_RO(energy4_overflow_raw, overflow, 3);
static SENSOR_DEVICE_ATTR_RO(energy5_overflow_raw, overflow, 4);
static struct attribute *aien_spbm_attrs[] = {
	&sensor_dev_attr_energy1_overflow_raw.dev_attr.attr,
	&sensor_dev_attr_energy2_overflow_raw.dev_attr.attr,
	&sensor_dev_attr_energy3_overflow_raw.dev_attr.attr,
	&sensor_dev_attr_energy4_overflow_raw.dev_attr.attr,
	&sensor_dev_attr_energy5_overflow_raw.dev_attr.attr, NULL
};
ATTRIBUTE_GROUPS(aien_spbm);
static const struct hwmon_ops aien_spbm_ops = {
	.is_visible = aien_spbm_visible, .read = aien_spbm_read,
	.read_string = aien_spbm_read_string,
};
static const struct hwmon_channel_info * const aien_spbm_info[] = {
	HWMON_CHANNEL_INFO(energy,
		HWMON_E_INPUT | HWMON_E_LABEL, HWMON_E_INPUT | HWMON_E_LABEL,
		HWMON_E_INPUT | HWMON_E_LABEL, HWMON_E_INPUT | HWMON_E_LABEL,
		HWMON_E_INPUT | HWMON_E_LABEL),
	HWMON_CHANNEL_INFO(power,
		HWMON_P_INPUT | HWMON_P_LABEL, HWMON_P_INPUT | HWMON_P_LABEL,
		HWMON_P_INPUT | HWMON_P_LABEL, HWMON_P_INPUT | HWMON_P_LABEL,
		HWMON_P_INPUT | HWMON_P_LABEL), NULL,
};
static const struct hwmon_chip_info aien_spbm_chip_info = {
	.ops = &aien_spbm_ops, .info = aien_spbm_info,
};

static int aien_spbm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct aien_spbm *spbm;
	struct resource *res;
	struct device *hwmon;
	int index, ret;
	BUILD_BUG_ON(sizeof(long) < 8);
	if (!ACPI_HANDLE(dev))
		return -ENODEV;
	index = spbm_resource_index(ACPI_HANDLE(dev));
	if (index < 0)
		return index;
	res = platform_get_resource(pdev, IORESOURCE_MEM, index);
	if (!res || res->start != SPBM_PHYS || resource_size(res) != SPBM_SIZE)
		return dev_err_probe(dev, -ENODEV, "SPBM resource contract mismatch\n");
	ret = spbm_check_registers(ACPI_HANDLE(dev), index);
	if (ret)
		return dev_err_probe(dev, ret, "SPBM register contract mismatch\n");
	spbm = devm_kzalloc(dev, sizeof(*spbm), GFP_KERNEL);
	if (!spbm)
		return -ENOMEM;
	spbm->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(spbm->base))
		return PTR_ERR(spbm->base);
	hwmon = devm_hwmon_device_register_with_info(dev, "aien_spbm", spbm,
		&aien_spbm_chip_info, aien_spbm_groups);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);
	dev_info(dev, "firmware contract verified; read-only SPBM telemetry ready\n");
	return 0;
}
static const struct acpi_device_id aien_spbm_ids[] = {
	{ "NVDA8800", 0 }, { }
};
MODULE_DEVICE_TABLE(acpi, aien_spbm_ids);
static struct platform_driver aien_spbm_driver = {
	.probe = aien_spbm_probe,
	.driver = {
		.name = "aien_spbm_readonly", .acpi_match_table = aien_spbm_ids,
	},
};
module_platform_driver(aien_spbm_driver);
MODULE_DESCRIPTION("Read-only, firmware-checked GB10 SPBM telemetry for R15");
MODULE_LICENSE("GPL");
