// SPDX-License-Identifier: GPL-2.0
/*
 * Fan control for the Acemagic W1 mini PC (ITE IT5570E embedded controller).
 *
 * The stock EC firmware pins the fan at 100 % duty and implements no fan curve.
 * The IT5570E lets the host reach EC-internal registers through I2EC (IT5570
 * datasheet V0.3.1, sections 6.3.2.9 and 7.23.3.5), which this firmware leaves
 * enabled read-write. This driver uses it to drive the fan PWM (PWM2/DCR2)
 * itself, following a temperature curve on the CPU temperature that the EC
 * mirrors in its ACPI space.
 *
 * pwm1_enable: 0 = full speed, 1 = manual (pwm1), 2 = automatic curve (default)
 *
 * Fail-safe: full speed at or above crit_temp or when the temperature can't be
 * read; the original duty is restored on unload, shutdown and suspend; the
 * duty register is re-checked every poll and rewritten if it changed.
 */

#include <linux/acpi.h>
#include <linux/devm-helpers.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/workqueue.h>

#define DRVNAME		"acemagic_w1_fan"

/* PNPCFG index/data ports and the depth-2 (I2EC) registers behind them */
#define SIO_IDX		0x4e
#define SIO_DAT		0x4f
#define SIO_CHIPID1	0x20
#define SIO_CHIPID2	0x21
#define SIO_D2ADR	0x2e
#define SIO_D2DAT	0x2f
#define I2EC_ADDR_L	0x10
#define I2EC_ADDR_H	0x11
#define I2EC_DATA	0x12

/* EC-side registers */
#define EC_CHIPID1	0x2000
#define EC_CHIPID2	0x2001
#define EC_SPCTRL1	0x200d
#define  SPCTRL1_I2EC_RW	0x03
#define PWM_DCR2	0x1804
#define PWM_PCSSGL	0x180c
#define  PCSSGL_CH2_MASK	0x30
#define  PCSSGL_CH2_CTR1	0x10
#define PWM_F2TLRR	0x1820
#define PWM_F2TMRR	0x1821
#define PWM_CTR1	0x1841

#define CHIP_ID1	0x55
#define CHIP_ID2	0x70

/* ACPI EC space: CPU temperature in degrees C */
#define ACPI_EC_CPU_TEMP	0x26

/* Tach sampling rate is FreqEC / 128 = 9.2 MHz / 128; the fan gives 2 pulses/rev */
#define RPM_FACTOR	(60 * (9200000 / 128) / 2)

#define NUM_POINTS	4
#define POLL_MS		1000
#define TEMP_HYST	3	/* degrees C before slowing down */
#define STEP_DOWN	8	/* max pwm decrease per poll */
#define PWM_FLOOR	51U	/* 20 %: the fan stalls at ~10 % and restarts poorly at 15 % */

static unsigned int min_pwm = 64;
module_param(min_pwm, uint, 0444);
MODULE_PARM_DESC(min_pwm, "Lowest pwm value ever set, 51-255 (default 64 = 25%)");

static unsigned int crit_temp = 85;
module_param(crit_temp, uint, 0444);
MODULE_PARM_DESC(crit_temp, "Full speed at or above this CPU temperature in C, 60-100 (default 85)");

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Skip the DMI (BIOS version) check");

struct w1fan_pdata {
	u8 ctr;		/* cycle time of the fan's PWM group (CTR1) */
	u8 orig_dcr;	/* duty found at load time, restored on exit */
};

struct w1fan {
	struct mutex lock;	/* serializes EC access and the fields below */
	struct delayed_work work;
	u8 ctr;
	u8 orig_dcr;
	long enable;
	unsigned int pwm;	/* manual value, or current target in auto mode */
	u8 point_temp[NUM_POINTS];
	u8 point_pwm[NUM_POINTS];
};

static const u8 default_temp[NUM_POINTS] = { 50, 60, 70, 80 };
static const u8 default_pwm[NUM_POINTS] = { 64, 102, 179, 255 };

/* Initial auto curve; must be NUM_POINTS non-decreasing values each */
static u8 curve_temp[NUM_POINTS] = { 50, 60, 70, 80 };
static unsigned int curve_temp_n;
module_param_array(curve_temp, byte, &curve_temp_n, 0444);
MODULE_PARM_DESC(curve_temp, "Initial curve temperatures in C, 4 values (default 50,60,70,80)");

static u8 curve_pwm[NUM_POINTS] = { 64, 102, 179, 255 };
static unsigned int curve_pwm_n;
module_param_array(curve_pwm, byte, &curve_pwm_n, 0444);
MODULE_PARM_DESC(curve_pwm, "Initial curve pwm values 0-255, 4 values (default 64,102,179,255)");

/* --- I2EC access; callers hold the muxed SIO region --- */

static int sio_enter(void)
{
	if (!request_muxed_region(SIO_IDX, 2, DRVNAME))
		return -EBUSY;
	return 0;
}

static void sio_exit(void)
{
	release_region(SIO_IDX, 2);
}

static u8 sio_read(u8 idx)
{
	outb(idx, SIO_IDX);
	return inb(SIO_DAT);
}

static void sio_write(u8 idx, u8 val)
{
	outb(idx, SIO_IDX);
	outb(val, SIO_DAT);
}

static void i2ec_set_addr(u16 addr)
{
	sio_write(SIO_D2ADR, I2EC_ADDR_H);
	sio_write(SIO_D2DAT, addr >> 8);
	sio_write(SIO_D2ADR, I2EC_ADDR_L);
	sio_write(SIO_D2DAT, addr & 0xff);
	sio_write(SIO_D2ADR, I2EC_DATA);
}

static u8 i2ec_read(u16 addr)
{
	i2ec_set_addr(addr);
	return sio_read(SIO_D2DAT);
}

static void i2ec_write(u16 addr, u8 val)
{
	i2ec_set_addr(addr);
	sio_write(SIO_D2DAT, val);
}

/* --- fan helpers --- */

static u8 pwm_to_dcr(const struct w1fan *f, unsigned int pwm)
{
	/* pwm 255 maps to DCR == CTR, which holds the output high (100 %) */
	return DIV_ROUND_CLOSEST(pwm * f->ctr, 255);
}

static unsigned int dcr_to_pwm(const struct w1fan *f, u8 dcr)
{
	return min(255U, DIV_ROUND_CLOSEST(dcr * 255U, f->ctr));
}

static int w1fan_cpu_temp(int *temp)
{
	u8 val;
	int ret;

	ret = ec_read(ACPI_EC_CPU_TEMP, &val);
	if (ret)
		return ret;
	if (val == 0 || val > 125)
		return -ERANGE;
	*temp = val;
	return 0;
}

static unsigned int w1fan_curve(const struct w1fan *f, int temp)
{
	int i;

	if (temp <= f->point_temp[0])
		return f->point_pwm[0];
	for (i = 1; i < NUM_POINTS; i++) {
		int t0 = f->point_temp[i - 1], t1 = f->point_temp[i];
		int p0 = f->point_pwm[i - 1], p1 = f->point_pwm[i];

		if (temp > t1)
			continue;
		if (t1 == t0)
			return p1;
		return p0 + (p1 - p0) * (temp - t0) / (t1 - t0);
	}
	return f->point_pwm[NUM_POINTS - 1];
}

/* Work out the wanted duty for the current mode and write it if the EC differs. */
static void w1fan_apply(struct w1fan *f)
{
	unsigned int up, down, target;
	u8 dcr;
	int temp;

	lockdep_assert_held(&f->lock);

	switch (f->enable) {
	case 0:
		dcr = f->ctr;
		break;
	case 1:
		dcr = pwm_to_dcr(f, clamp(f->pwm, min_pwm, 255U));
		break;
	default:
		if (w1fan_cpu_temp(&temp) || temp >= (int)crit_temp) {
			target = 255;
		} else {
			up = w1fan_curve(f, temp);
			down = w1fan_curve(f, temp + TEMP_HYST);
			if (up > f->pwm)
				target = up;
			else if (down < f->pwm)
				target = max(down, f->pwm > STEP_DOWN ? f->pwm - STEP_DOWN : 0U);
			else
				target = f->pwm;
		}
		f->pwm = clamp(target, min_pwm, 255U);
		dcr = pwm_to_dcr(f, f->pwm);
		break;
	}

	if (sio_enter())
		return;	/* retried on the next poll */
	if (i2ec_read(PWM_DCR2) != dcr)
		i2ec_write(PWM_DCR2, dcr);
	sio_exit();
}

static void w1fan_restore(struct w1fan *f)
{
	mutex_lock(&f->lock);
	if (!sio_enter()) {
		i2ec_write(PWM_DCR2, f->orig_dcr);
		sio_exit();
	}
	mutex_unlock(&f->lock);
}

static void w1fan_work(struct work_struct *work)
{
	struct w1fan *f = container_of(to_delayed_work(work), struct w1fan, work);

	mutex_lock(&f->lock);
	w1fan_apply(f);
	mutex_unlock(&f->lock);
	schedule_delayed_work(&f->work, msecs_to_jiffies(POLL_MS));
}

/* --- hwmon --- */

static int w1fan_read_rpm(struct w1fan *f, long *val)
{
	u8 msb, lsb = 0, msb2 = 0;
	int i, ret;
	u16 count;

	mutex_lock(&f->lock);
	ret = sio_enter();
	if (ret)
		goto out;
	/* LSB/MSB aren't latched together over I2EC; retry if MSB moved */
	for (i = 0; i < 3; i++) {
		msb = i2ec_read(PWM_F2TMRR);
		lsb = i2ec_read(PWM_F2TLRR);
		msb2 = i2ec_read(PWM_F2TMRR);
		if (msb == msb2)
			break;
	}
	sio_exit();
	count = msb2 << 8 | lsb;
	*val = (count && count != 0xffff) ? RPM_FACTOR / count : 0;
out:
	mutex_unlock(&f->lock);
	return ret;
}

static int w1fan_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct w1fan *f = dev_get_drvdata(dev);
	int temp, ret;

	switch (type) {
	case hwmon_fan:
		return w1fan_read_rpm(f, val);
	case hwmon_temp:
		ret = w1fan_cpu_temp(&temp);
		if (ret)
			return ret;
		*val = temp * 1000;
		return 0;
	case hwmon_pwm:
		mutex_lock(&f->lock);
		if (attr == hwmon_pwm_enable)
			*val = f->enable;
		else
			*val = f->enable == 0 ? 255 : f->pwm;
		mutex_unlock(&f->lock);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int w1fan_write(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long val)
{
	struct w1fan *f = dev_get_drvdata(dev);
	int ret = 0;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	mutex_lock(&f->lock);
	if (attr == hwmon_pwm_enable) {
		if (val < 0 || val > 2) {
			ret = -EINVAL;
			goto out;
		}
		f->enable = val;
	} else {
		if (val < 0 || val > 255) {
			ret = -EINVAL;
			goto out;
		}
		if (f->enable != 1) {
			ret = -EBUSY;
			goto out;
		}
		f->pwm = clamp((unsigned int)val, min_pwm, 255U);
	}
	w1fan_apply(f);
out:
	mutex_unlock(&f->lock);
	return ret;
}

static umode_t w1fan_is_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	return type == hwmon_pwm ? 0644 : 0444;
}

static const struct hwmon_channel_info * const w1fan_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops w1fan_ops = {
	.is_visible = w1fan_is_visible,
	.read = w1fan_read,
	.write = w1fan_write,
};

static const struct hwmon_chip_info w1fan_chip_info = {
	.ops = &w1fan_ops,
	.info = w1fan_info,
};

/* pwm1_auto_point[1-4]_{temp,pwm}; points must stay non-decreasing */

static ssize_t point_temp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w1fan *f = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", f->point_temp[to_sensor_dev_attr(attr)->index] * 1000);
}

static ssize_t point_temp_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct w1fan *f = dev_get_drvdata(dev);
	int i = to_sensor_dev_attr(attr)->index;
	long val;
	int ret;

	ret = kstrtol(buf, 10, &val);
	if (ret)
		return ret;
	val = DIV_ROUND_CLOSEST(val, 1000);
	if (val < 20 || val > 100)
		return -EINVAL;

	mutex_lock(&f->lock);
	if ((i > 0 && val < f->point_temp[i - 1]) ||
	    (i < NUM_POINTS - 1 && val > f->point_temp[i + 1]))
		ret = -EINVAL;
	else
		f->point_temp[i] = val;
	mutex_unlock(&f->lock);
	return ret ? ret : count;
}

static ssize_t point_pwm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w1fan *f = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", f->point_pwm[to_sensor_dev_attr(attr)->index]);
}

static ssize_t point_pwm_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct w1fan *f = dev_get_drvdata(dev);
	int i = to_sensor_dev_attr(attr)->index;
	u8 val;
	int ret;

	ret = kstrtou8(buf, 10, &val);
	if (ret)
		return ret;

	mutex_lock(&f->lock);
	if ((i > 0 && val < f->point_pwm[i - 1]) ||
	    (i < NUM_POINTS - 1 && val > f->point_pwm[i + 1]))
		ret = -EINVAL;
	else
		f->point_pwm[i] = val;
	mutex_unlock(&f->lock);
	return ret ? ret : count;
}

static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point1_temp, point_temp, 0);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point2_temp, point_temp, 1);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point3_temp, point_temp, 2);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point4_temp, point_temp, 3);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point1_pwm, point_pwm, 0);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point2_pwm, point_pwm, 1);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point3_pwm, point_pwm, 2);
static SENSOR_DEVICE_ATTR_RW(pwm1_auto_point4_pwm, point_pwm, 3);

static struct attribute *w1fan_attrs[] = {
	&sensor_dev_attr_pwm1_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point3_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point4_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point3_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point4_pwm.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(w1fan);

/* --- platform driver --- */

static void w1fan_restore_action(void *data)
{
	w1fan_restore(data);
}

static int w1fan_probe(struct platform_device *pdev)
{
	const struct w1fan_pdata *pd = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct device *hwmon;
	struct w1fan *f;
	int ret;

	f = devm_kzalloc(dev, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;

	mutex_init(&f->lock);
	f->ctr = pd->ctr;
	f->orig_dcr = pd->orig_dcr;
	f->enable = 2;
	f->pwm = dcr_to_pwm(f, f->orig_dcr);
	memcpy(f->point_temp, curve_temp, sizeof(f->point_temp));
	memcpy(f->point_pwm, curve_pwm, sizeof(f->point_pwm));
	platform_set_drvdata(pdev, f);

	/* devm actions run in reverse: hwmon goes, then the work stops, then we restore */
	ret = devm_add_action_or_reset(dev, w1fan_restore_action, f);
	if (ret)
		return ret;
	ret = devm_delayed_work_autocancel(dev, &f->work, w1fan_work);
	if (ret)
		return ret;

	hwmon = devm_hwmon_device_register_with_info(dev, DRVNAME, f,
						     &w1fan_chip_info, w1fan_groups);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	schedule_delayed_work(&f->work, 0);
	dev_info(dev, "IT5570E fan control active (CTR=0x%02x, original duty 0x%02x)\n",
		 f->ctr, f->orig_dcr);
	return 0;
}

static void w1fan_shutdown(struct platform_device *pdev)
{
	struct w1fan *f = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&f->work);
	w1fan_restore(f);
}

static int w1fan_suspend(struct device *dev)
{
	struct w1fan *f = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&f->work);
	w1fan_restore(f);
	return 0;
}

static int w1fan_resume(struct device *dev)
{
	struct w1fan *f = dev_get_drvdata(dev);

	schedule_delayed_work(&f->work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(w1fan_pm, w1fan_suspend, w1fan_resume);

static struct platform_driver w1fan_driver = {
	.driver = {
		.name = DRVNAME,
		.pm = pm_sleep_ptr(&w1fan_pm),
	},
	.probe = w1fan_probe,
	.shutdown = w1fan_shutdown,
};

static struct platform_device *w1fan_pdev;

static bool __init w1fan_dmi_match(void)
{
	const char *bios = dmi_get_system_info(DMI_BIOS_VERSION);

	return bios && strstarts(bios, "PHXPM7B0");
}

static bool __init w1fan_curve_valid(void)
{
	int i;

	/* a partial array would mix given and default values */
	if ((curve_temp_n && curve_temp_n != NUM_POINTS) ||
	    (curve_pwm_n && curve_pwm_n != NUM_POINTS))
		return false;
	for (i = 0; i < NUM_POINTS; i++) {
		if (curve_temp[i] < 20 || curve_temp[i] > 100)
			return false;
		if (i && (curve_temp[i] < curve_temp[i - 1] || curve_pwm[i] < curve_pwm[i - 1]))
			return false;
	}
	return true;
}

static int __init w1fan_detect(struct w1fan_pdata *pd)
{
	int ret;

	ret = sio_enter();
	if (ret)
		return ret;

	ret = -ENODEV;
	if (sio_read(SIO_CHIPID1) != CHIP_ID1 || sio_read(SIO_CHIPID2) != CHIP_ID2)
		goto out;
	if (i2ec_read(EC_CHIPID1) != CHIP_ID1 || i2ec_read(EC_CHIPID2) != CHIP_ID2) {
		pr_info(DRVNAME ": IT5570E found but I2EC is disabled\n");
		goto out;
	}
	if ((i2ec_read(EC_SPCTRL1) & SPCTRL1_I2EC_RW) != SPCTRL1_I2EC_RW) {
		pr_info(DRVNAME ": I2EC is not read-write\n");
		goto out;
	}
	if ((i2ec_read(PWM_PCSSGL) & PCSSGL_CH2_MASK) != PCSSGL_CH2_CTR1) {
		pr_info(DRVNAME ": unexpected PWM2 clock group\n");
		goto out;
	}
	pd->ctr = i2ec_read(PWM_CTR1);
	pd->orig_dcr = i2ec_read(PWM_DCR2);
	if (!pd->ctr || pd->orig_dcr > pd->ctr) {
		pr_info(DRVNAME ": unexpected CTR1/DCR2 0x%02x/0x%02x\n", pd->ctr, pd->orig_dcr);
		goto out;
	}
	ret = 0;
out:
	sio_exit();
	return ret;
}

static int __init w1fan_init(void)
{
	struct w1fan_pdata pd;
	int ret;

	if (!force && !w1fan_dmi_match())
		return -ENODEV;

	min_pwm = clamp(min_pwm, PWM_FLOOR, 255U);
	crit_temp = clamp(crit_temp, 60U, 100U);
	if (!w1fan_curve_valid()) {
		pr_warn(DRVNAME ": invalid curve_temp/curve_pwm, using defaults\n");
		memcpy(curve_temp, default_temp, sizeof(curve_temp));
		memcpy(curve_pwm, default_pwm, sizeof(curve_pwm));
	}

	ret = w1fan_detect(&pd);
	if (ret)
		return ret;

	ret = platform_driver_register(&w1fan_driver);
	if (ret)
		return ret;

	w1fan_pdev = platform_device_register_data(NULL, DRVNAME, PLATFORM_DEVID_NONE,
						   &pd, sizeof(pd));
	if (IS_ERR(w1fan_pdev)) {
		platform_driver_unregister(&w1fan_driver);
		return PTR_ERR(w1fan_pdev);
	}
	return 0;
}

static void __exit w1fan_exit(void)
{
	platform_device_unregister(w1fan_pdev);
	platform_driver_unregister(&w1fan_driver);
}

module_init(w1fan_init);
module_exit(w1fan_exit);

MODULE_DESCRIPTION("Acemagic W1 (ITE IT5570E) fan control");
MODULE_LICENSE("GPL");
