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
#include <stdio.h>
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <inttypes.h>
#include <math.h>
#include <time.h>

#include "rio_misc.h"
#include "rio_route.h"
#include "tok_parse.h"
#include "rio_mport_lib.h"
#include "string_util.h"
#include "goodput_cli.h"


#include "libtime_utils.h"
#include "librsvdmem.h"
#include "liblog.h"
#include "assert.h"
#include "math_util.h"
#include "RapidIO_Error_Management_API.h"
#include "pw_handling.h"
#include "CPS1848.h"
#include "CPS1616.h"
#include "RXS2448.h"
#include "Tsi721.h"
#include "cps_event_test.h"
#include "umd_worker.h"
#include "goodput_umd_cli.h"


#ifdef __cplusplus
extern "C" {
#endif

struct UMDEngineInfo umd_engine[MAX_UMD_CH];


// Parse the token as a boolean value. The range of the token is restricted
// to the numeric values of 0 (false) and 1 (true)
static int gp_parse_bool(struct cli_env *env, char *tok, const char *name, uint16_t *boo)
{
    if (tok_parse_ushort(tok, boo, 0, 1, 0)) {
        LOGMSG(env, "\n");
        LOGMSG(env, TOK_ERR_USHORT_MSG_FMT, name, 0, 1);
        return 1;
    }
    return 0;
}

// Parse the token ensuring it is within the provided range. Further ensure it
// is a power of 2
static int gp_parse_ull_pw2(struct cli_env *env, char *tok, const char *name,
        uint64_t *value, uint64_t min, uint64_t max)
{
    if (tok_parse_ulonglong(tok, value, min, max, 0)) {
        LOGMSG(env, "\n");
        LOGMSG(env, TOK_ERR_ULONGLONG_HEX_MSG_FMT, name, min, max);
        goto err;
    }

    if ((*value - 1) & *value) {
        LOGMSG(env, "\n%s must be a power of 2\n", name);
        goto err;
    }

    return 0;
err:
    return 1;
}


static int gp_parse_did(struct cli_env *env, char *tok, did_val_t *did_val)
{
    if (tok_parse_did(tok, did_val, 0)) {
        LOGMSG(env, "\n");
        LOGMSG(env, "<did> must be between 0 and 0xff\n");
        return 1;
    }
    return 0;
}


int umdDmaNumCmd(struct cli_env *env, int UNUSED(argc), char **argv)
{
    uint16_t idx;
    did_val_t did_val;
    uint64_t rio_addr;
    uint64_t buf_sz;
    uint16_t wr;
    uint32_t num_trans;
    char *user_data_p = NULL;
    struct UMDEngineInfo *engine_p;
    
    int n = 0;

    if(tok_parse_long(argv[n++], &idx, 0, MAX_UMD_CH_IDX, 0))
    {   
        LOGMSG(env, "\n");
        LOGMSG(env, TOK_ERR_LONG_MSG_FMT,"<idx>", 0, MAX_UMD_CH_IDX);
        goto exit;
    }
    
    if (gp_parse_did(env, argv[n++], &did_val))
    {
        goto exit;
    }

    
    if (tok_parse_ulonglong(argv[n++], &rio_addr, 1, UINT64_MAX, 0))
    {
        LOGMSG(env, "\n");
        LOGMSG(env, TOK_ERR_ULONGLONG_HEX_MSG_FMT, "<rio_addr>",
                (uint64_t)1, (uint64_t)UINT64_MAX);
        goto exit;
    }   

    if (gp_parse_ull_pw2(env, argv[n++], "<buf_size>", &buf_sz, 1, UINT32_MAX))
    {
        goto exit;
    }
    
    if (gp_parse_bool(env, argv[n++], "<wr>", &wr)) 
    {
        goto exit;
    }

    
    
    if ((argc > 5 && tok_parse_ul(argv[n++], &num_trans, 0)) 
    {
        LOGMSG(env, "\n");
        LOGMSG(env, TOK_ERR_UL_HEX_MSG_FMT, "<num>");
        goto exit;
    }

    if( argc > 6)
    {
        user_data_p = argv[n];
    }

    engine_p = &umd_engine[idx];
    if (engine_p->stat == ENGINE_READY)
    {
        engine_p->rio_addr = rio_addr;
        engine_p->buf_size = buf_sz;
        engine_p->max_iter = num_trans;
        engine_p->wr = wr;
        engine_p->ch_idx = idx;
        umd_open(engine_p);
    }    
    
exit:
    return 0;

}
    
int umdOpen(struct cli_env *env, int UNUSED(argc), char **argv)
{
    int idx;
    int n = 0;
    int mport_id;
    struct UMDEngineInfo *engine_p;

    if(tok_parse_long(argv[n++], &idx, 0, MAX_UMD_CH_IDX, 0))
    {   
        LOGMSG(env, "\n");
        LOGMSG(env, TOK_ERR_LONG_MSG_FMT,"<idx>", 0, MAX_UMD_CH_IDX);
        goto exit;
    }

    if(tok_parse_mport_id(argv[n++], &mport_id, 0))
    {
        goto exit;
    }

    engine_p = &umd_engine[idx];
    if (engine_p->stat == ENGINE_UNALLOCATED)
    {
        engine_p->mport_id = mport_id;
        engine_p->ch_idx = idx;
        umd_open(engine_p);
    }
        

exit:
    return 0;
}


int umdConfig(struct struct cli_env *env, int UNUSED(argc), char **argv)
{
    int idx;
    int n = 0;
    uint64_t ib_size;
    uint64_t ib_rio_addr = RIO_MAP_ANY_ADDR;
    uint64_t ib_phys_addr= RIO_MAP_ANY_ADDR;
    struct UMDEngineInfo *engine_p;

    if(tok_parse_long(argv[0], &idx, 0, MAX_UMD_CH_IDX, 0))
    {   
        LOGMSG(env, "\n");
        LOGMSG(env, TOK_ERR_LONG_MSG_FMT,"<idx>", 0, MAX_UMD_CH_IDX);
        goto exit;
    }

    if (gp_parse_ull_pw2(env, argv[1], "<size>", &ib_size, FOUR_KB,
            4 * SIXTEEN_MB)) {
        goto exit;
    }

    if(argc > 2  && 
        (tok_parse_ulonglong(argv[2], &ib_rio_addr, 1, UINT64_MAX, 0)))
    {
        LOGMSG(env, "\n");
        LOGMSG(env, TOK_ERR_ULONGLONG_HEX_MSG_FMT, "<rio_addr>",
                (uint64_t )1, (uint64_t)UINT64_MAX);
        
        ib_rio_addr = RIO_MAP_ANY_ADDR;
    }

    if ((ib_rio_addr != RIO_MAP_ANY_ADDR) && ((ib_size-1) & ib_rio_addr)) {
        LOGMSG(env, "\n<addr> not aligned with <size>\n");
        goto exit;
    }

    
    engine_p = &umd_engine[idx];
    if (engine_p->stat == ENGINE_UNCONFIGURED)
    {
        engine_p->ib_rio_addr = ib_rio_addr;
        engine_p->ib_byte_cnt = ib_size;
        umd_config(engine_p);
    }


exit: 

    return 0;
}

int umdStart(struct struct cli_env *env, int UNUSED(argc), char **argv)
{
    int idx;
    int n = 0;
    struct UMDEngineInfo *engine_p;

    if(tok_parse_long(argv[n++], &idx, 0, MAX_UMD_CH_IDX, 0))
    {   
        LOGMSG(env, "\n");
        LOGMSG(env, TOK_ERR_LONG_MSG_FMT,"<idx>", 0, MAX_UMD_CH_IDX);
        goto exit;
    }

    engine_p = &umd_engine[idx];
    if (engine_p->stat == ENGINE_UNCONFIGURED)
    {
        umd_start(engine_p);
    }
        

exit:
    return 0;
}

int umdStop(struct struct cli_env *env, int UNUSED(argc), char **argv)
{
    int idx;
    int n = 0;
    struct UMDEngineInfo *engine_p;

    if(tok_parse_long(argv[n++], &idx, 0, MAX_UMD_CH_IDX, 0))
    {   
        LOGMSG(env, "\n");
        LOGMSG(env, TOK_ERR_LONG_MSG_FMT,"<idx>", 0, MAX_UMD_CH_IDX);
        goto exit;
    }

    engine_p = &umd_engine[idx];
    if (engine_p->stat == ENGINE_READY)
    {
        umd_stop(engine_p);
    }

exit:
     
    return 0;
}

int umdClose(struct struct cli_env *env, int UNUSED(argc), char **argv)
{
    int idx;
    int n = 0;
    struct UMDEngineInfo *engine_p;

    if(tok_parse_long(argv[n++], &idx, 0, MAX_UMD_CH_IDX, 0))
    {   
        LOGMSG(env, "\n");
        LOGMSG(env, TOK_ERR_LONG_MSG_FMT,"<idx>", 0, MAX_UMD_CH_IDX);
        goto exit;
    }

    engine_p = &umd_engine[idx];
    if (engine_p->stat == ENGINE_CONFIGURED
        || engine_p->stat == ENGINE_UNCONFIGURED)
    {
        umd_close(engine_p);
    }

exit:
     
    return 0;
}

    

#ifdef __cplusplus
}
#endif


