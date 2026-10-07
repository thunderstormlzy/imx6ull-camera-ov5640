#ifndef CAMERA_TIMER_EVENTS_H
#define CAMERA_TIMER_EVENTS_H

/*
 * 用户程序和内核模块通过 /dev/camera_timer 传递一个字节的事件。
 * 事件本身不携带图片数据，只表示“为什么要唤醒保存线程”。
 */
/* 一小时 timer_list 到期后产生的事件。 */
#define CAMERA_TIMER_EVENT_TIME 'T'
/* KEY0 线程检测到按键后写入的事件。 */
#define CAMERA_TIMER_EVENT_KEY 'K'
/* 程序退出时写入，只用于唤醒阻塞在 read() 上的保存线程。 */
#define CAMERA_TIMER_EVENT_QUIT 'Q'

#endif
