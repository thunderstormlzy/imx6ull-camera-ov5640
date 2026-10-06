#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define CAMERA_KEY_NAME "key"
#define CAMERA_KEY_VALUE 1
#define CAMERA_KEY_DEBOUNCE_MS 20

struct camera_key_device {
	dev_t devid;
	struct cdev cdev;
	struct class *class;
	struct device *device;
	struct device_node *node;
	int gpio;
	int irq;
	struct timer_list debounce_timer;
	wait_queue_head_t wait;
	spinlock_t lock;
	int pressed;
	int event;
};

static struct camera_key_device camera_key;

static void camera_key_debounce(unsigned long argument)
{
	struct camera_key_device *device = (struct camera_key_device *)argument;
	unsigned long flags;
	int wake = 0;
	int value;

	value = gpio_get_value(device->gpio);
	spin_lock_irqsave(&device->lock, flags);
	if (value == 0) {
		device->pressed = 1;
	} else if (device->pressed) {
		device->pressed = 0;
		device->event = CAMERA_KEY_VALUE;
		wake = 1;
	}
	spin_unlock_irqrestore(&device->lock, flags);

	if (wake)
		wake_up_interruptible(&device->wait);
}

static irqreturn_t camera_key_irq(int irq, void *argument)
{
	struct camera_key_device *device = argument;

	(void)irq;
	mod_timer(&device->debounce_timer,
		  jiffies + msecs_to_jiffies(CAMERA_KEY_DEBOUNCE_MS));
	return IRQ_HANDLED;
}

static int camera_key_open(struct inode *inode, struct file *file)
{
	(void)inode;
	file->private_data = &camera_key;
	return 0;
}

static ssize_t camera_key_read(struct file *file, char __user *buffer,
				       size_t count, loff_t *offset)
{
	struct camera_key_device *device = file->private_data;
	unsigned long flags;
	unsigned char value;
	int result;

	(void)offset;
	if (count < sizeof(value))
		return -EINVAL;

	result = wait_event_interruptible(device->wait, device->event != 0);
	if (result)
		return result;

	spin_lock_irqsave(&device->lock, flags);
	value = device->event;
	device->event = 0;
	spin_unlock_irqrestore(&device->lock, flags);

	if (copy_to_user(buffer, &value, sizeof(value)) != 0)
		return -EFAULT;
	return sizeof(value);
}

static const struct file_operations camera_key_operations = {
	.owner = THIS_MODULE,
	.open = camera_key_open,
	.read = camera_key_read,
};

static int camera_key_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	int result;

	if (!node)
		return -ENODEV;
	camera_key.node = node;
	/* 从匹配成功的设备树节点读取 key_gpio，而不是写死 GPIO 编号。 */
	camera_key.gpio = of_get_named_gpio(node, "key_gpio", 0);
	if (!gpio_is_valid(camera_key.gpio)) {
		result = -EINVAL;
		goto clear_node;
	}
	result = gpio_request(camera_key.gpio, CAMERA_KEY_NAME);
	if (result)
		goto clear_node;
	result = gpio_direction_input(camera_key.gpio);
	if (result)
		goto free_gpio;

	camera_key.irq = irq_of_parse_and_map(node, 0);
	if (camera_key.irq <= 0) {
		result = -EINVAL;
		goto free_gpio;
	}

	init_waitqueue_head(&camera_key.wait);
	spin_lock_init(&camera_key.lock);
	init_timer(&camera_key.debounce_timer);
	camera_key.debounce_timer.function = camera_key_debounce;
	camera_key.debounce_timer.data = (unsigned long)&camera_key;

	result = alloc_chrdev_region(&camera_key.devid, 0, 1, CAMERA_KEY_NAME);
	if (result)
		goto dispose_irq;
	cdev_init(&camera_key.cdev, &camera_key_operations);
	result = cdev_add(&camera_key.cdev, camera_key.devid, 1);
	if (result)
		goto unregister_region;

	camera_key.class = class_create(THIS_MODULE, CAMERA_KEY_NAME);
	if (IS_ERR(camera_key.class)) {
		result = PTR_ERR(camera_key.class);
		goto delete_cdev;
	}
	camera_key.device = device_create(camera_key.class, NULL,
					 camera_key.devid, NULL, CAMERA_KEY_NAME);
	if (IS_ERR(camera_key.device)) {
		result = PTR_ERR(camera_key.device);
		goto destroy_class;
	}

	result = request_irq(camera_key.irq, camera_key_irq,
				     IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				     CAMERA_KEY_NAME, &camera_key);
	if (result)
		goto destroy_device;
	platform_set_drvdata(pdev, &camera_key);

	return 0;

destroy_device:
	device_destroy(camera_key.class, camera_key.devid);
destroy_class:
	class_destroy(camera_key.class);
delete_cdev:
	cdev_del(&camera_key.cdev);
unregister_region:
	unregister_chrdev_region(camera_key.devid, 1);
dispose_irq:
	irq_dispose_mapping(camera_key.irq);
free_gpio:
	gpio_free(camera_key.gpio);
clear_node:
	camera_key.node = NULL;
	return result;
}

static int camera_key_remove(struct platform_device *pdev)
{
	struct camera_key_device *device = platform_get_drvdata(pdev);

	del_timer_sync(&device->debounce_timer);
	free_irq(device->irq, device);
	irq_dispose_mapping(device->irq);
	device_destroy(device->class, device->devid);
	class_destroy(device->class);
	cdev_del(&device->cdev);
	unregister_chrdev_region(device->devid, 1);
	gpio_free(device->gpio);
	platform_set_drvdata(pdev, NULL);
	device->node = NULL;
	return 0;
}

static const struct of_device_id camera_key_of_match[] = {
	/* 只有 compatible 匹配时，platform 总线才会调用 probe。 */
	{ .compatible = "lzy,camera-key" },
	{ }
};
MODULE_DEVICE_TABLE(of, camera_key_of_match);

static struct platform_driver camera_key_driver = {
	.probe = camera_key_probe,
	.remove = camera_key_remove,
	.driver = {
		.name = "camera-key",
		.of_match_table = camera_key_of_match,
	},
};

module_platform_driver(camera_key_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lzy");
MODULE_DESCRIPTION("GPIO key driver for camera image saving");
