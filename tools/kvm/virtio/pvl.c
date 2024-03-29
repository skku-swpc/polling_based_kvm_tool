
#include "kvm/virtio-pvl.h"
#include "kvm/virtio-pci-dev.h"

#include "kvm/virtio.h"
#include "kvm/util.h"
#include "kvm/atomic.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"
#include "kvm/guest_compat.h"
#include "kvm/kvm-ipc.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_pvl.h>

#include <linux/kernel.h>
#include <linux/list.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <stdio.h>

#define NUM_VIRT_QUEUES		1
#define VIRTIO_PVL_QUEUE_SIZE	128
//#define VIRTIO_PVL_COMMAND	0

#define QUEUE_SIZE 70

struct pvl_command_t {
	unsigned short command;
	unsigned short undone;
	unsigned long args[5];
	unsigned long result; 
};

struct pvl_queue_t {
	int head; 
	int tail;
	struct pvl_command_t pages[QUEUE_SIZE];
};

struct pvl_dev {
	struct list_head	list;
	struct virtio_device	vdev;
	u32	features;

	/* virtio queue */
	struct virt_queue	vqs[NUM_VIRT_QUEUES];
	struct pvl_queue_t *rq;
	struct virtio_pvl_config config;
};

static struct pvl_dev pdev;
static int compat_id = -1;

static int pvl_dequeue(struct pvl_queue_t *q, struct pvl_command_t **command) 
{
	int index;
	int next;

	__asm__ __volatile__ ("sfence":::"memory");
	do {
		if (q->head == q->tail)
			return -1;

		index = q->head;
		next = (index + 1) % QUEUE_SIZE;
	} while (atomic_cmpxchg((void*)&q->head, index, next) != index);
	
	*command = &q->pages[index];
	
	while((*command)->command==0){
		__asm__ __volatile__ ("rep; nop":::"memory");
	}

	if (q->pages[index].undone == 0) {
		printf("already done entry. idx:%d\n", index);
		return -1;
	}

	return index;
}

pthread_t iocore = (unsigned)-1;

/*
void printbuf(void *buf, size_t size);
void printbuf(void *buf, size_t size) {
	int i;
	unsigned char* ch=buf;

	for(i=0; i<(int)size; i++) {
		if(i%48==0) 	{
			if(i!=0)	putc('\n', stderr);
		} else if(i%4==0)	putc(' ', stderr);

		fprintf(stderr, "%02X", ch[i]);
	}
	putc('\n', stderr);
	fflush(stderr);
}

ssize_t PGSIZE=0x1000;
void *guest_addr_to_host(struct kvm *kvm, unsigned long offset);
void *guest_addr_to_host(struct kvm *kvm, unsigned long offset) {
	//__asm__ __volatile__ ("mfence":::"memory");
	unsigned long ptr;
	do {
		ptr = (unsigned long)guest_flat_to_host(kvm, (u64)offset);

		fprintf(stderr, "guest:%lX, host:%lX\n", ptr&(PGSIZE-1),offset&(PGSIZE-1));
		if((ptr&(PGSIZE-1)) != (offset&(PGSIZE-1))) {
			fprintf(stderr, "[%s]error! guest:%lX, host:%lX\n",__FUNCTION__, offset, ptr);
			continue;
		} else {
			break;
		}
	} while((ptr&(PGSIZE-1)) != (offset&(PGSIZE-1)));
	return (void *)ptr;
}
*/

#define PVL_QUEUE_DROPCACHE 8

static void* iocore_main(void *param) {
	cpu_set_t mask;
	struct pvl_command_t *cmd; 
	struct kvm* kvm = (struct kvm*)param;
	void* gaddr;
	int queue_idx;

	CPU_ZERO(&mask);
	CPU_SET(2, &mask);
	sched_setaffinity(0, sizeof(mask), &mask);

	printf("pvl_queue size:%ld\n", sizeof(struct pvl_queue_t));
	
	while(1) {
		queue_idx=pvl_dequeue(pdev.rq, &cmd);
		if(queue_idx<0) {		
			__asm__ __volatile__ ("rep; nop":::"memory");
		} else {
			//printf("in iocore_main, cmd:%d", cmd->command);
			switch(cmd->command) {
				case PVL_QUEUE_OPEN:
					gaddr = guest_flat_to_host(kvm, cmd->args[0]); 			
					int flag = cmd->args[1];
					//flag |= /*(O_DIRECT | O_SYNC |*/ (O_NOATIME | O_LARGEFILE);
					//const char *path = "/mnt/sdb2/image.img";
					const char *path = "/dev/sdb3";
					cmd->result = open64(path, flag);
					printf("image file open - name:%s, open:%s, flag:(%ld->)%d, fd:%ld\n", (char*)gaddr, path,cmd->args[1], flag,cmd->result);
					break;
				case PVL_QUEUE_CLOSE:
					cmd->result = close(cmd->args[0]);
					printf("image file close - fd:%ld\n", cmd->args[0]);
					break;
				case PVL_QUEUE_READ: //3
					gaddr = guest_flat_to_host(kvm, cmd->args[1]);
					cmd->result = read(cmd->args[0], gaddr, cmd->args[2]);
					if(cmd->args[3]>0) {
						gaddr = guest_flat_to_host(kvm, cmd->args[3]);
						cmd->result += read(cmd->args[0], gaddr, cmd->args[4]);
					}
					//fprintf(stderr, "[read] size:%ld\n", cmd->args[2]+cmd->args[4]);
					/*
					while(cmd->result!=cmd->args[2]) {
						remain = cmd->args[2]-cmd->result;
						rtn = read(cmd->args[0], gaddr+cmd->result, remain);
						if(rtn<=0) {
							perror("pvl read, retry");
							break;
						}
						printf("READ RETRY!! rtn:%ld\n", rtn);
						cmd->result+=rtn;
					}*/
					//fprintf(stderr, "[read, %lx, %p]", cmd->args[1], gaddr);
					//printbuf(gaddr, 8);
					break;
				case PVL_QUEUE_WRITE: //4
					gaddr = guest_flat_to_host(kvm, cmd->args[1]);
					cmd->result = write(cmd->args[0], gaddr, cmd->args[2]);
					if(cmd->args[3]>0) {
						gaddr = guest_flat_to_host(kvm, cmd->args[3]);
						cmd->result += write(cmd->args[0], gaddr, cmd->args[4]);
					}
					//fprintf(stderr, "[write] size:%ld\n", cmd->args[2]+cmd->args[4]);
					break;
				case PVL_QUEUE_LSEEK: //5
					//fprintf(stderr, "[lseek] offset:%ld,", cmd->args[1]);
					cmd->result = lseek64(cmd->args[0], cmd->args[1], cmd->args[2]);
					break;
				case PVL_QUEUE_FSYNC:
					cmd->result = fsync(cmd->args[0]);
					break;
				case PVL_QUEUE_FSTAT:
					gaddr = guest_flat_to_host(kvm, cmd->args[1]);
					cmd->result = fstat(cmd->args[0], gaddr);
					break;
				case PVL_QUEUE_DROPCACHE:
					printf("Droping caches..\n");
					cmd->result=system("sync && echo 3 > /proc/sys/vm/drop_caches");
					break;
				default:
					printf("[IOcore]Unknown command:%d\n", cmd->command);
					cmd->result = -111;
			}
			//if(cmd->result <= 0) 	printf("!!result:%ld\n", cmd->result);
			cmd->command=0;
			cmd->undone=0;
		}
	}
	return NULL;
}


static bool virtio_pvl_do_io_request(struct kvm *kvm, struct pvl_dev *pdev, u32 qaddr)
{
	pdev->rq = guest_flat_to_host(kvm, qaddr << VIRTIO_PVL_PFN_SHIFT);

	if(iocore==(unsigned)-1) {
		printf("io-thread is created\n");
		pthread_create(&iocore, NULL, iocore_main, (void*)kvm);
	}
	return true;
}

static void virtio_pvl_do_io(struct kvm *kvm, u32 qaddr)
{
	virtio_pvl_do_io_request(kvm, &pdev, qaddr);
}

static u8 *get_config(struct kvm *kvm, void *dev)
{
	struct pvl_dev *pdev = dev;

	return ((u8 *)(&pdev->config));
}

static u32 get_host_features(struct kvm *kvm, void *dev)
{
	return 0;
	//return 1 << VIRTIO_BALLOON_F_STATS_VQ;
}

static void set_guest_features(struct kvm *kvm, void *dev, u32 features)
{
	struct pvl_dev *pdev = dev;

	pdev->features = features;
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq, u32 page_size, u32 align, u32 pfn)
{
	struct pvl_dev *pdev = dev;
	struct virt_queue *queue;
	void *p;

	compat__remove_message(compat_id);

	queue		= &pdev->vqs[vq];
	queue->pfn	= pfn;
	p		= virtio_get_vq(kvm, queue->pfn, page_size);

	vring_init(&queue->vring, VIRTIO_PVL_QUEUE_SIZE, p, align);

	return 0;
}

static int make_pvlq(struct kvm *kvm, void *dev, u32 pvlq)
{
	//struct pvl_dev *pdev = dev;
	virtio_pvl_do_io(kvm, pvlq);
	return 0;
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return 0;
}

static int get_pfn_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct pvl_dev *pdev = dev;

	return pdev->vqs[vq].pfn;
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return VIRTIO_PVL_QUEUE_SIZE;
}

static int set_size_vq(struct kvm *kvm, void *dev, u32 vq, int size)
{
	/* FIXME: dynamic */
	return size;
}

struct virtio_ops pvl_dev_virtio_ops = (struct virtio_ops) {
	.get_config		= get_config,
	.get_host_features	= get_host_features,
	.set_guest_features	= set_guest_features,
	.init_vq		= init_vq,
	.notify_vq		= notify_vq,
	.make_pvlq		= make_pvlq,
	.get_pfn_vq		= get_pfn_vq,
	.get_size_vq	= get_size_vq,
	.set_size_vq	= set_size_vq,
};

int virtio_pvl__init(struct kvm *kvm)
{
	//bdev.stat_waitfd	= eventfd(0, 0);
	//memset(&bdev.config, 0, sizeof(struct virtio_balloon_config));

	virtio_init(kvm, &pdev, &pdev.vdev, &pvl_dev_virtio_ops,
		    VIRTIO_DEFAULT_TRANS, PCI_DEVICE_ID_VIRTIO_PVL,
		    VIRTIO_ID_PVL, PCI_CLASS_PVL);

	if (compat_id == -1)
		compat_id = virtio_compat_add_message("virtio-pvl", "CONFIG_VIRTIO_PVL");

	return 0;
}
virtio_dev_init(virtio_pvl__init);

int virtio_pvl__exit(struct kvm *kvm)
{
	return 0;
}
virtio_dev_exit(virtio_pvl__exit);
