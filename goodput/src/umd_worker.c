/*
****************************************************************************
Copyright (c) 2019, Renesas Electronics Corporation.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*************************************************************************
*/

#include <stdint.h>
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include <pthread.h>

#include <sched.h>

#include "rio_route.h"
#include "string_util.h"
#include "libcli.h"
#include "rio_mport_lib.h"
#include "liblog.h"

#include "tsi721_umd.h"
#include "libtime_utils.h"
#include "umd_worker.h"
#include "pw_handling.h"
#include "goodput.h"
#include "Tsi721.h"
#include "CPS1848.h"
#include "rio_misc.h"
#include "did.h"


#ifdef __cplusplus
extern "C" {
#endif

#define PATTERN_SIZE  8
#define UDM_QUEUE_SIZE  (8 * 8192)
#define INIT_CRC32 0xFF00FF00

#define ADDR_L(x,y) ((uint64_t)((uint64_t)x + (uint64_t)y))
#define ADDR_P(x,y) ((void *)((uint64_t)x + (uint64_t)y))


struct data_prefix
{
    uint32_t trans_nth; /*current transaction index*/
    uint32_t CRC32;
    uint64_t xfer_offset; /* Transfer occurs at xfer_offset bytes from the start of the target buffer */
    uint64_t xfer_size; /* Transfer consists of xfer_size*/
    uint8_t pattern[PATTERN_SIZE]; /*Predefined pattern*/
};

struct data_suffix
{
    uint8_t pattern[PATTERN_SIZE]; /*Predefined pattern*/
};

struct data_status
{
    int trans_nth; /*current transaction index*/
    int xfer_check; /* 0 – waiting for transfer check, 1 – passed 2 – failed */
    uint8_t pattern[PATTERN_SIZE]; /*Predefined pattern*/
};

static int umd_allo_ibw(struct UMDEngineInfo *info, int index)
{
    struct DmaTransfer *dma_trans_p = &info->dma_trans[index];
    int rc;

    LOGMSG(info->env, "INFO: ib_rio_addr 0x%lx, ib_size 0x%lx\n", dma_trans_p->ib_rio_addr, dma_trans_p->ib_byte_cnt);

    if (!dma_trans_p->ib_byte_cnt || dma_trans_p->ib_valid)
    {
        LOGMSG(info->env, "FAILED: window size of 0 or ibwin already exists\n");
        return -1;
    }

    rc = rio_ibwin_map(info->engine.dev_fd, &dma_trans_p->ib_rio_addr,
                    dma_trans_p->ib_byte_cnt, &dma_trans_p->ib_handle);
    LOGMSG(info->env, "INFO: ib_handle 0x%lx\n", dma_trans_p->ib_handle);
    
    if (rc)
    {
        LOGMSG(info->env, "FAILED: rio_ibwin_map rc %d:%s\n",
                    rc, strerror(errno));
        return -1;
    }
    if (dma_trans_p->ib_handle == 0)
    {
        LOGMSG(info->env, "FAILED: rio_ibwin_map failed silently with info->ib_handle==0!\n");
        return -1;
    }


    dma_trans_p->ib_ptr = NULL;
    dma_trans_p->ib_ptr = mmap(NULL, dma_trans_p->ib_byte_cnt, PROT_READ | PROT_WRITE,
                MAP_SHARED, info->engine.dev_fd, dma_trans_p->ib_handle);
    // rc = riomp_dma_map_memory(info->mp_h, info->ib_byte_cnt,
    //              info->ib_handle, &info->ib_ptr);
    if (dma_trans_p->ib_ptr == MAP_FAILED)
    {
        dma_trans_p->ib_ptr = NULL;
    }

    if (NULL == dma_trans_p->ib_ptr)
    {
        LOGMSG(info->env, "FAILED: riomp_dma_map_memory errno %d:%s\n",
                    errno, strerror(errno));
        rio_ibwin_free(info->engine.dev_fd, &dma_trans_p->ib_handle);
        return -1;
    }

    memset(dma_trans_p->ib_ptr, 0x0, dma_trans_p->ib_byte_cnt);

    dma_trans_p->ib_valid = 1;

    return 0;
}

static int umd_free_ibw(struct UMDEngineInfo *info, int index)
{
    struct DmaTransfer *dma_trans_p = &info->dma_trans[index];
    int ret = -1, rc;

    if (!dma_trans_p->ib_valid)
    {
        LOGMSG(info->env, "No valid inbound window. User thread %d\n", index);
        return ret;
    }

    if (dma_trans_p->ib_ptr && dma_trans_p->ib_valid) {
        rc = munmap(dma_trans_p->ib_ptr, dma_trans_p->ib_byte_cnt);
        dma_trans_p->ib_ptr = NULL;
        if (rc)
        {
            LOGMSG(info->env, "munmap ib rc %d: %s\n",
                rc, strerror(errno));
            ret = -1;
        }
    }

    rc = rio_ibwin_free(info->engine.dev_fd, &dma_trans_p->ib_handle);
    if (rc)
    {
        LOGMSG(info->env, "FAILED: rio_ibwin_free rc %d:%s\n",
                    rc, strerror(errno));
        ret = -1;
    }

    dma_trans_p->ib_valid = 0;
    dma_trans_p->ib_handle = 0;

    return 0;
}

static int umd_allo_tx_buf(struct UMDEngineInfo *info, int index)
{
    struct DmaTransfer *dma_trans_p = &info->dma_trans[index];
    int rc, ret = 0;


    dma_trans_p->tx_mem_h = RIO_MAP_ANY_ADDR;

    rc = rio_dbuf_alloc(info->engine.dev_fd, dma_trans_p->buf_size, &dma_trans_p->tx_mem_h);

    if (rc)
    {
        LOGMSG(info->env, "FAILED: riomp_dma_dbuf_alloc tx buffer rc %d:%s\n",
                        rc, strerror(errno));
        ret = -1;
        goto exit;
    }

    dma_trans_p->tx_ptr = NULL;
    dma_trans_p->tx_ptr = mmap(NULL, dma_trans_p->buf_size,
            PROT_READ | PROT_WRITE, MAP_SHARED,
            info->engine.dev_fd, dma_trans_p->tx_mem_h);

    if (dma_trans_p->tx_ptr == MAP_FAILED)
    {
        dma_trans_p->tx_ptr = NULL;
    }

    if (NULL == dma_trans_p->tx_ptr)
    {
        LOGMSG(info->env, "FAILED: mmap tx buffer errno %d:%s\n",
                 errno, strerror(errno));
        ret = -1;
        goto exit;
    }

exit:
    return ret;
}

static int umd_free_tx_buf(struct UMDEngineInfo *info, int index)
{
    struct DmaTransfer *dma_trans_p = &info->dma_trans[index];

    if (dma_trans_p->tx_ptr)
    {
        munmap(dma_trans_p->tx_ptr, dma_trans_p->buf_size);
        dma_trans_p->tx_ptr = NULL;
    }

    if (dma_trans_p->tx_mem_h)
    {
        rio_dbuf_free(info->engine.dev_fd, &dma_trans_p->tx_mem_h);
        dma_trans_p->tx_mem_h = 0;
    }

    return 0;
}

static int umd_allo_queue_mem(struct UMDEngineInfo *info)
{
    int rc, ret = 0;

    info->queue_mem_h = RIO_MAP_ANY_ADDR;

    rc = rio_dbuf_alloc(info->engine.dev_fd, UDM_QUEUE_SIZE, &info->queue_mem_h);

    if (rc)
    {
        LOGMSG(info->env, "FAILED: riomp_dma_dbuf_alloc queue buffer rc %d: %d %s\n",
                        rc, errno, strerror(errno));
        ret = -1;
    }
    else
    {
        LOGMSG(info->env, "INFO: DMA queue handle 0x%lx\n", info->queue_mem_h);
    }

    return ret;
}

static int umd_free_queue_mem(struct UMDEngineInfo *info)
{
    if (info->queue_mem_h)
    {
        rio_dbuf_free(info->engine.dev_fd, &info->queue_mem_h);
        info->queue_mem_h = 0;
    }

    return 0;
}

static uint32_t crc32_table[256];

static uint32_t crc32(uint32_t crc, void *buffer, uint32_t size)  
{  
    uint32_t i; 
    uint8_t *data_p = (uint8_t *)buffer;
    for (i = 0; i < size; i++)
    {  
        crc = crc32_table[(crc ^ data_p[i]) & 0xff] ^ (crc >> 8);  
    }  
    return crc ;  
}

static void umd_init_crc_table(void)  
{  
    uint32_t c;  
    uint32_t i, j;  
      
    for (i = 0; i < 256; i++)
    {  
        c = (uint32_t)i;  
        for (j = 0; j < 8; j++) 
        {  
            if (c & 1)
            {
                c = 0xedb88320L ^ (c >> 1);  
            }
            else
            {
                c = c >> 1;
            }
        }  
        crc32_table[i] = c;  
    }  
}  

static void umd_copy_xfer_data(void *buf, uint32_t size, uint64_t user_data)
{
    uint64_t *data_p =  (uint64_t *)buf; //Assume 64 byte alignment.
    uint32_t count = size / 4;
    uint32_t i;
    for(i = 0; i < count; i++)
    {
        *data_p =  user_data;
        data_p++;
    }
}

int umd_init_engine(struct UMDEngineInfo *info)
{
    memset(info, 0x0, sizeof(struct UMDEngineInfo));

    umd_init_crc_table();

    return 0;
}

int umd_open(struct UMDEngineInfo *info)
{
    if( info->stat ==  ENGINE_UNALLOCATED)
    {
        if(tsi721_umd_open(&(info->engine), info->mport_id) == 0)
        {
            info->stat = ENGINE_UNCONFIGURED;
            return 0;
        }
        else
        {
            LOGMSG(info->env, "FAILED: driver returns error.\n");
        }
    }
    else
    {
        LOGMSG(info->env, "FAILED: Engine is in state %d\n", info->stat);
    }

    return -1;
}

int umd_config(struct UMDEngineInfo *info)
{
    if(info->stat == ENGINE_UNCONFIGURED)
    {
        if(!umd_allo_queue_mem(info))
        {
            LOGMSG(info->env, "SUCC: queue mem is allocated.\n");

            if(!tsi721_umd_queue_config_multi(&(info->engine), info->chan_mask, (void *)info->queue_mem_h, UDM_QUEUE_SIZE))
            {
                LOGMSG(info->env, "SUCC: UDM queue is configured. Channel mask 0x%x\n", info->chan_mask);  
                info->stat = ENGINE_CONFIGURED;
                return 0;
            }
            else
            {
                LOGMSG(info->env, "FAILED: Engine configure error. Channel mask 0x%x\n", info->chan_mask);
                umd_free_queue_mem(info);
            }
        }
        else
        {
            LOGMSG(info->env, "FAILED: allocate queue memory error.\n");
        }
    }
    else
    {
        LOGMSG(info->env, "FAILED: Engine is in state %d\n", info->stat);
    }

    return -1;
}

int umd_start(struct UMDEngineInfo *info)
{
    if(info->stat == ENGINE_CONFIGURED)
    {
        if(tsi721_umd_start(&(info->engine)) == 0)
        {
            info->stat = ENGINE_READY;
            LOGMSG(info->env, "SUCC: UMD engine started.\n");
            return 0;
        }
        else
        {
            LOGMSG(info->env, "FAILED: UMD start returns failure\n");
        }
    }
    else
    {
        LOGMSG(info->env, "FAILED: Engine is in state %d\n", info->stat);
    }

    return 0;
}

int umd_stop(struct UMDEngineInfo *info)
{
    if(info->stat == ENGINE_READY)
    {
        if(tsi721_umd_stop(&(info->engine)) == 0)
        {
            info->stat = ENGINE_CONFIGURED;
            return 0;
        }
        else
        {
            LOGMSG(info->env, "FAILED: UMD stop returns failure\n");
        }
    }
    else
    {
        LOGMSG(info->env, "FAILED: Engine is in state %d\n", info->stat);
    }

    return -1;
}

int umd_close(struct UMDEngineInfo *info)
{
    if(info->stat == ENGINE_CONFIGURED || info->stat ==  ENGINE_UNCONFIGURED)
    {
        umd_free_queue_mem(info);

        if(tsi721_umd_close(&(info->engine)) == 0)
        {
            info->stat = ENGINE_UNALLOCATED;
            return 0;
        }
        else
        {
            LOGMSG(info->env, "FAILED: UMD close returns failure\n");
        }
    }
    else
    {
        LOGMSG(info->env, "FAILED: Engine is in state %d\n", info->stat);
    }
    return -1;
}

int umd_dma_num_cmd(struct UMDEngineInfo *info, int index)
{
    struct DmaTransfer *dma_trans_p = &info->dma_trans[index];
    data_status *status;
    data_prefix *prefix;
    void *xfer_p;
    data_suffix *suffix = NULL;
    int32_t ret = 0, rc;
    uint32_t i;
    uint32_t loops;

     if(umd_allo_ibw(info, index))
     {
         ret = -1;
         goto exit;
     }

     if(umd_allo_tx_buf(info,index))
     {
         ret  = -1;
         goto exit;
     }

    if (!dma_trans_p->rio_addr || !dma_trans_p->buf_size)
    {
        LOGMSG(info->env, "FAILED: rio_addr, buf_size is 0!\n");
        ret = -1;
        goto exit;
    }

    if(dma_trans_p->num_trans != 0)
    {
        loops = dma_trans_p->num_trans;
    }
    else
    {
        loops = 0xFFFFFFFF;
    }

    if(dma_trans_p->wr)
    {
        prefix = (data_prefix*)dma_trans_p->tx_ptr;
        status = (data_status*)dma_trans_p->ib_ptr;

        for(i=0; i<loops; i++)
        {

            memset(status, 0, sizeof(data_status));
            memset(prefix, 0, sizeof(data_prefix));
            prefix->trans_nth = i+1;
            prefix->pattern[0] = 0x12;
            prefix->pattern[1] = 0x34;
            prefix->pattern[2] = 0x56;
            prefix->pattern[3] = 0x78;
            prefix->pattern[4] = 0x9A;
            prefix->pattern[5] = 0xBC;
            prefix->pattern[6] = 0xDE;
            prefix->pattern[7] = 0xF1;
            prefix->xfer_offset = sizeof(data_prefix);
            prefix->xfer_size = dma_trans_p->buf_size - sizeof(data_prefix) - sizeof(data_suffix);

            xfer_p = (void *)((uint8_t *)(dma_trans_p->tx_ptr) + prefix->xfer_offset);
            umd_copy_xfer_data(xfer_p, prefix->xfer_size, dma_trans_p->user_data);
            prefix->CRC32 = crc32(INIT_CRC32, xfer_p, prefix->xfer_size);

            suffix = (data_suffix*)((uint64_t)(dma_trans_p->tx_ptr)  +   prefix->xfer_offset + prefix->xfer_size);
            memset(suffix, 0, sizeof(data_suffix));
            suffix->pattern[0] = 0x1a;
            suffix->pattern[1] = 0x2b;
            suffix->pattern[2] = 0x3c;
            suffix->pattern[3] = 0x4d;
            suffix->pattern[4] = 0xa1;
            suffix->pattern[5] = 0xb2;
            suffix->pattern[6] = 0xc3;
            suffix->pattern[7] = 0xd4;

            rc = tsi721_umd_send(&(info->engine), (void *)dma_trans_p->tx_mem_h, dma_trans_p->buf_size, dma_trans_p->rio_addr, dma_trans_p->dest_id);
            if(rc == 0)
            {
                while(status->xfer_check == 0)
                {
                    sleep(0.25);
                }

                if(status->xfer_check == 2 ||
                    status->trans_nth != prefix->trans_nth ||
                    status->pattern[0] != 0x11 ||
                    status->pattern[1] != 0x22 ||
                    status->pattern[2] != 0x33 ||
                    status->pattern[3] != 0x44 ||
                    status->pattern[4] != 0x55 ||
                    status->pattern[5] != 0x66 ||
                    status->pattern[6] != 0x77 ||
                    status->pattern[7] != 0x88)
                {
                    LOGMSG(info->env, "FAILED: Writer status(from Reader) pattern check error: loop %u\n" 
                        "xfer_check %d, data 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
                        i,
                        status->xfer_check,
                        status->pattern[0],
                        status->pattern[1],
                        status->pattern[2],
                        status->pattern[3],
                        status->pattern[4],
                        status->pattern[5],
                        status->pattern[6],
                        status->pattern[7]);
                    ret  = -1;
                    break;
                }
            }
            else
            {
                LOGMSG(info->env, "FAILED: Writer UMD send failed\n");
                ret = -1;
                break;
            }
        }
    }
    else
    {
        status = (data_status*)dma_trans_p->tx_ptr;
        prefix = (data_prefix*)dma_trans_p->ib_ptr;

        for(i=0; i<loops; i++)
        {
            while(prefix->trans_nth != i+1 )
            {
                sleep(0.25);
            }

            status->pattern[0] = 0x11;
            status->pattern[1] = 0x22;
            status->pattern[2] = 0x33;
            status->pattern[3] = 0x44;
            status->pattern[4] = 0x55;
            status->pattern[5] = 0x66;
            status->pattern[6] = 0x77;
            status->pattern[7] = 0x88;
            status->trans_nth = prefix->trans_nth;

            if( prefix->pattern[0] == 0x12 &&
                prefix->pattern[1] == 0x34 &&
                prefix->pattern[2] == 0x56 &&
                prefix->pattern[3] == 0x78 &&
                prefix->pattern[4] == 0x9A &&
                prefix->pattern[5] == 0xBC &&
                prefix->pattern[6] == 0xDE &&
                prefix->pattern[7] == 0xF1)
            {
               suffix = (data_suffix*)((uint64_t)(dma_trans_p->ib_ptr) + prefix->xfer_offset + prefix->xfer_size);

               while(suffix->pattern[0] == 00 &&
                    suffix->pattern[1] == 00 &&
                    suffix->pattern[2] == 00 &&
                    suffix->pattern[3] == 00 &&
                    suffix->pattern[4] == 00 &&
                    suffix->pattern[5] == 00 &&
                    suffix->pattern[6] == 00 &&
                    suffix->pattern[7] == 00)
               {
                   sleep(0.25);
               }
                    
               if(suffix->pattern[0] == 0x1a &&
                    suffix->pattern[1] == 0x2b &&
                    suffix->pattern[2] == 0x3c &&
                    suffix->pattern[3] == 0x4d &&
                    suffix->pattern[4] == 0xa1 &&
                    suffix->pattern[5] == 0xb2 &&
                    suffix->pattern[6] == 0xc3 &&
                    suffix->pattern[7] == 0xd4)
               {
                   xfer_p = (void *)((uint8_t *)(dma_trans_p->ib_ptr) + prefix->xfer_offset);   
                   if(prefix->CRC32 != crc32(INIT_CRC32, xfer_p, prefix->xfer_size))
                   {
                       status->xfer_check = -1;
                       LOGMSG(info->env, "FAILED: Reader user data CRC32 validation error\n");
                   }
                   else
                   {
                       status->xfer_check = 1;
                   }
               }
               else
               {
                   ret = -1;
                   
                   LOGMSG(info->env, "FAILED: Reader suffix validation error, loop %u\n" 
                    "data 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
                        i,
                        suffix->pattern[0],
                        suffix->pattern[1],
                        suffix->pattern[2],
                        suffix->pattern[3],
                        suffix->pattern[4],
                        suffix->pattern[5],
                        suffix->pattern[6],
                        suffix->pattern[7]);
                   
                   status->xfer_check = 2;
               }
            }
            else
            {
                ret = -1;
            
                LOGMSG(info->env, "FAILED: Reader prefix validation error, loop %u\n"
                    "data 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
                    i,
                    prefix->pattern[0],
                    prefix->pattern[1],
                    prefix->pattern[2],
                    prefix->pattern[3],
                    prefix->pattern[4],
                    prefix->pattern[5],
                    prefix->pattern[6],
                    prefix->pattern[7]);
                
                status->xfer_check = 2;
            }

            memset(prefix, 0, sizeof(data_prefix));
            memset(suffix, 0, sizeof(data_suffix));

            rc = tsi721_umd_send(&(info->engine), (void *)dma_trans_p->tx_mem_h, dma_trans_p->buf_size, dma_trans_p->rio_addr, dma_trans_p->dest_id);
            if (rc !=0 )                
            {
                ret = -1;
                LOGMSG(info->env, "FAILED: Reader loop %u, UMD send failed\n", i);
            }

            if(ret)
            {
                break;
            }
        }
    }



exit:
    umd_free_ibw(info,index);
    umd_free_tx_buf(info, index);
    if( ret == 0)
    {
        LOGMSG(info->env,"INFO: Writer/Reader %d completed %u loops of UDM DMA test successfully!!!\n", dma_trans_p->wr, loops);
    }
    return ret;
}

int umd_goodput(struct UMDEngineInfo *info, int index)
{
    struct DmaTransfer *dma_trans_p = &info->dma_trans[index];
    int32_t ret = 0, rc;
    uint32_t loops = 0;
    uint64_t count = 0;

    if(umd_allo_tx_buf(info,index))
    {
        ret  = -1;
        goto exit;
    }

    if (!dma_trans_p->rio_addr || !dma_trans_p->buf_size || !dma_trans_p->acc_size)
    {
        LOGMSG(info->env, "FAILED: rio_addr, buf_size or access size is 0!\n");
        ret = -1;
        goto exit;
    }

    clock_gettime(CLOCK_MONOTONIC, &dma_trans_p->st_time);
    for(count = 0; count < dma_trans_p->buf_size; count += dma_trans_p->acc_size)
    {

        loops++;
        rc = tsi721_umd_send(&(info->engine), 
            ADDR_P(dma_trans_p->tx_mem_h,count), 
            dma_trans_p->acc_size,
            ADDR_L(dma_trans_p->rio_addr, count),
            dma_trans_p->dest_id
            );
        if(rc)
        {
            ret = -1;
            LOGMSG(info->env, "FAILED: in the %u loop of DMA send in\n", loops);
            break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &dma_trans_p->end_time);

exit:
    umd_free_tx_buf(info, index);
    if( ret == 0)
    {
        struct timespec elapsed_time;
        float MBps;
        float Gbps;
        uint64_t nsec;
        uint64_t byte_cnt;
    
        LOGMSG(info->env, "INFO: Throughput completed. %u loops of UDM DMA send!!!\n", loops);
        LOGMSG(info->env, "INFO: Total tranfer size: 0x%lx bytes\n", byte_cnt = loops * dma_trans_p->acc_size );
        LOGMSG(info->env, "INFO: Each DMA access size: 0x%lx bytes\n", dma_trans_p->acc_size );
        LOGMSG(info->env, "INFO: Start time sec:%lu ns:%lu\n", dma_trans_p->st_time.tv_sec, dma_trans_p->st_time.tv_nsec);
        LOGMSG(info->env, "INFO: End time sec:%lu ns:%lu\n",dma_trans_p->end_time.tv_sec, dma_trans_p->end_time.tv_nsec);

        elapsed_time = time_difference(dma_trans_p->st_time, dma_trans_p->end_time);
        nsec = elapsed_time.tv_nsec + (elapsed_time.tv_sec * 1000000000);

        //1000 or 1024???
        MBps = (float)(byte_cnt / (1024*1024)) / ((float)nsec / 1000000000.0);
        Gbps = (MBps * 1024.0 * 1024.0 * 8.0) / 1000000000.0;
        LOGMSG(info->env, "INFO: duration ns:%lu\n", nsec);
        LOGMSG(info->env, "INFO: Goodput %4.4fMBps, %2.4fGbp\n", MBps, Gbps);
    }   
    return ret;
}


#ifdef __cplusplus
}
#endif



