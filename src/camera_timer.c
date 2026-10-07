#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "camera_timer_events.h"

#define CAMERA_TIMER_NAME "camera_timer"
#define CAMERA_TIMER_COUNT 1
#define CAMERA_TIMER_SECONDS (60 * 60)

struct camera_timer_device {
	dev_t devid;
	struct cdev cdev;
	struct class *class;
	struct device *device;
	struct timer_list timer;
	wait_queue_head_t wait;
	spinlock_t lock;
	unsigned char event;
};

static struct camera_timer_device camera_timer;

static void camera_timer_function(unsigned long argument)
{
	struct camera_timer_device *device = (struct camera_timer_device *)argument;
	unsigned long flags;

	spin_lock_irqsave(&device->lock, flags);
	device->event = CAMERA_TIMER_EVENT_TIME;
	spin_unlock_irqrestore(&device->lock, flags);
	wake_up_interruptible(&device->wait);

	/* 内核定时器是一次性的，重新设置后才会每小时再次触发。 */
	mod_timer(&device->timer, jiffies + CAMERA_TIMER_SECONDS * HZ);
}

static int camera_timer_open(struct inode *inode, struct file *file)
{
	file->private_data = &camera_timer;
	return 0;
}

static ssize_t camera_timer_read(struct file *file, char __user *buffer,
				 size_t count, loff_t *offset)
{
	struct camera_timer_device *device = file->private_data;
	unsigned long flags;
	unsigned char event;
	int result;

	if (count < sizeof(event))
		return -EINVAL;
	result = wait_event_interruptible(device->wait, device->event != 0);
	if (result)
		return result;

	spin_lock_irqsave(&device->lock, flags);
	event = device->event;
	device->event = 0;
	spin_unlock_irqrestore(&device->lock, flags);
	if (copy_to_user(buffer, &event, sizeof(event)) != 0)
		return -EFAULT;
	return sizeof(event);
}

static ssize_t camera_timer_write(struct file *file, const char __user *buffer,
				  size_t count, loff_t *offset)
{
	struct camera_timer_device *device = file->private_data;
	unsigned long flags;
	unsigned char event;

	if (count < sizeof(event))
		return -EINVAL;
	if (copy_from_user(&event, buffer, sizeof(event)) != 0)
		return -EFAULT;

	spin_lock_irqsave(&device->lock, flags);
	device->event = event;
	spin_unlock_irqrestore(&device->lock, flags);
	wake_up_interruptible(&device->wait);
	return sizeof(event);
}

static const struct file_operations camera_timer_operations = {
	.owner = THIS_MODULE,
	.open = camera_timer_open,
	.read = camera_timer_read,
	.write = camera_timer_write,
};

static int __init camera_timer_init(void)
{
	int result;

	result = alloc_chrdev_region(&camera_timer.devid, 0,
				    CAMERA_TIMER_COUNT, CAMERA_TIMER_NAME);
	if (result < 0)
		return result;

	cdev_init(&camera_timer.cdev, &camera_timer_operations);
	result = cdev_add(&camera_timer.cdev, camera_timer.devid,
			  CAMERA_TIMER_COUNT);
	if (result < 0)
		goto unregister_region;

	camera_timer.class = class_create(THIS_MODULE, CAMERA_TIMER_NAME);
	if (IS_ERR(camera_timer.class)) {
		result = PTR_ERR(camera_timer.class);
		goto delete_cdev;
	}
	camera_timer.device = device_create(camera_timer.class, NULL,
					    camera_timer.devid, NULL,
					    CAMERA_TIMER_NAME);
	if (IS_ERR(camera_timer.device)) {
		result = PTR_ERR(camera_timer.device);
		goto destroy_class;
	}

	init_waitqueue_head(&camera_timer.wait);
	spin_lock_init(&camera_timer.lock);
	init_timer(&camera_timer.timer);
	camera_timer.timer.function = camera_timer_function;
	camera_timer.timer.data = (unsigned long)&camera_timer;
	mod_timer(&camera_timer.timer,
		  jiffies + CAMERA_TIMER_SECONDS * HZ);
	return 0;

destroy_class:
	class_destroy(camera_timer.class);
delete_cdev:
	cdev_del(&camera_timer.cdev);
unregister_region:
	unregister_chrdev_region(camera_timer.devid, CAMERA_TIMER_COUNT);
	return result;
}

static void __exit camera_timer_exit(void)
{
	del_timer_sync(&camera_timer.timer);
	device_destroy(camera_timer.class, camera_timer.devid);
	class_destroy(camera_timer.class);
	cdev_del(&camera_timer.cdev);
	unregister_chrdev_region(camera_timer.devid, CAMERA_TIMER_COUNT);
}

module_init(camera_timer_init);
module_exit(camera_timer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lzy");
MODULE_DESCRIPTION("One-hour timer event device for camera image saving");
