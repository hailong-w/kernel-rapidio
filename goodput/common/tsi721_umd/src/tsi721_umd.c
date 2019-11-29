#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "tsi721_umd.h"

#define CFG_MEM_SIZE (512*1024) // TBD: check this size

// TBD - check normal defaults on these
#define DEFAULT_REQUEST_Q_SIZE    (16*1024*1024)
#define DEFAULT_COMPLETION_Q_SIZE (16*1024*1024)

static int32_t map_bar0(struct tsi721_umd* h, int32_t mport_id);

static int32_t map_bar0(struct tsi721_umd* h, int32_t mport_id)
{
	int32_t fd;
	char bar_filename[256];
	void* ptr;

	snprintf(bar_filename,256,"/sys/class/rapidio_port/rapidio%d/device/resource0",mport_id);

	fd = open(bar_filename, O_RDWR | O_SYNC | O_CLOEXEC);

	if (fd < 0)
	{
		ERRMSG("Failed to open fd to mport %d bar 0 at filename %s, error %d %s\n",mport_id,bar_filename, errno, strerror(errno));
		return -1;
	}

	h->regs_fd = fd;

	ptr = mmap(NULL, CFG_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (ptr == MAP_FAILED)
	{
		ERRMSG("Failed to mmap %d bytes of bar0 space, error %d %s\n",CFG_MEM_SIZE,errno,strerror(errno));
		return -1;
	}

	h->all_regs = ptr;

	return 0;
}


int32_t tsi721_umd_open(struct tsi721_umd* h, uint32_t mport_id)
{
	assert(h->state == TSI721_UMD_STATE_UNALLOCATED);
	
	memset(h, 0, sizeof(*h));

	// Get device handle
	h->dev_fd = rio_mport_open(mport_id, 0);
	if (h->dev_fd <= 0)
	{
		ERRMSG("Fail to open mport dev\n");
		return -1;
	}
	
	// Get pointer to BAR0 configuration space
	if (map_bar0(h,mport_id) < 0)
	{
		ERRMSG("Failed to map registers to process mem\n");
		return -1;
	}

	h->state = TSI721_UMD_STATE_UNCONFIGURED;

	return 0;
}

int32_t tsi721_umd_queue_config(struct tsi721_umd* h, uint8_t channel_num, void* queue_mem_phys, uint32_t queue_mem_size)
{
	uint32_t page_size = sysconf(_SC_PAGE_SIZE);
	struct dma_channel* chan;
	int32_t mem_fd;

	if (!h)
		return -1;
	
	if (channel_num >= TSI721_DMA_CHNUM)
	{
		ERRMSG("Illegal channel number %d > %d\n",channel_num,TSI721_DMA_CHNUM-1);
		return -1;
	}

	if (h->chan_mask & (1<<channel_num))
	{
		ERRMSG("Channel %d already configured\n", channel_num);
		return -1;
	}
	
	chan = &h->chan[channel_num];

	// TBD: use user-config sizes for queues instead of a fixed default
	if (queue_mem_size < (DEFAULT_REQUEST_Q_SIZE + DEFAULT_COMPLETION_Q_SIZE))
	{
		ERRMSG("Queue memory size must be at least %d\n",DEFAULT_REQUEST_Q_SIZE + DEFAULT_COMPLETION_Q_SIZE);
		return -1;
	}
	else if	((queue_mem_size & (page_size-1)) != 0)
	{
		ERRMSG("Invalid queue memory size %d, must be a multiple of %d\n",queue_mem_size,page_size);
		return -1;
	}

	if (((uintptr_t)queue_mem_phys & (page_size-1)) != 0)
	{
		ERRMSG("Invalid queue memory address %p, must be page aligned to %x\n",queue_mem_phys,page_size);
		return -1;
	}
	
	// Note: Access to /dev/mem requires root access
	// If not ok, will need a separate process to open this mapping on boot
	chan->request_q_phys = queue_mem_phys;
	mem_fd = open("/dev/mem", O_SYNC);
	if (mem_fd <= 0)
	{
		ERRMSG("Failed to open /dev/mem (not running as root?)\n");
		return -1;
	}

	chan->request_q = mmap(NULL, queue_mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, mem_fd, (size_t)queue_mem_phys);

	if (chan->request_q == MAP_FAILED)
	{
		ERRMSG("Failed to mmap the queue memory\n");
		return -1;
	}

	chan->completion_q = chan->request_q + DEFAULT_REQUEST_Q_SIZE;
	chan->completion_q_phys = chan->request_q + DEFAULT_REQUEST_Q_SIZE;
	chan->reg_base = (void*)((uintptr_t)h->all_regs + TSI721_DMAC_BASE(channel_num));

	h->chan_mask |= (1<<channel_num);

	return 0;
}

int32_t tsi721_umd_queue_config_multi(struct tsi721_umd* h, uint8_t channel_mask, void* phys_mem, uint32_t phys_mem_size)
{
	int32_t i, ret, fail=0;
	uintptr_t ptr = (uintptr_t)phys_mem;
	const uint32_t queue_size = DEFAULT_REQUEST_Q_SIZE + DEFAULT_COMPLETION_Q_SIZE;

	assert(h->state == TSI721_UMD_STATE_UNCONFIGURED);

	if (phys_mem_size < queue_size * TSI721_DMA_CHNUM)
	{
		ERRMSG("Error: %d allocated physical memory size is insufficent",phys_mem_size);
		return -1;
	}

	for (i=0; i<TSI721_DMA_CHNUM; i++)
	{
		if ((1<<i) & channel_mask)
		{
			ret = tsi721_umd_queue_config(h,i,(void*)ptr,queue_size);
			if (ret < 0)
			{
				ERRMSG("Failed to configure queue %d\n",i);
				fail = 1;
			}
			else
			{
				printf("Success configure queue %d\n",i);
			}
			ptr += queue_size;
		}
	}

	if (fail)
		return -1;

	h->state = TSI721_UMD_STATE_CONFIGURED;
	return 0;
}

int32_t tsi721_umd_start(struct tsi721_umd* h)
{
	int32_t ret;

	// Allocate channel dispatch mutex
	ret = pthread_mutex_init(&h->channel_mutex, NULL);
	if (ret < 0)
	{
		ERRMSG("Error initializing mutex, err %d %s\n",ret,strerror(ret));
		return -1;
	}

	h->state = TSI721_UMD_STATE_READY;

	return 0;
}