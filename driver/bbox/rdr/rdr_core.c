/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022-2023. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * Description:
 * Author: huawei
 * Create: 2022-08-13
 */

#include "rdr_core.h"

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/semaphore.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/securec.h>

#include "bootparam/bbox_boot_param.h"
#include "common/bbox_print.h"
#include "common/bbox_sys_api.h"
#include "config/bbox_config.h"
#include "rdr_common.h"
#include "rdr_dfx_core.h"
#include "rdr_dump_core.h"
#include "rdr_exception_core.h"
#include "rdr_field_core.h"
#include "rdr_module_core.h"
#include "register/bbox_register.h"
#include "bbox_inner.h"

STATIC struct semaphore g_rdr_sem;
STATIC LIST_HEAD(g_rdr_syserr_list);
STATIC DEFINE_SPINLOCK(g_rdr_syserr_list_lock);
STATIC bool g_init_done = false;

/*
 * @brief       : put the exception into list
 * @param [in]  : dev_id                        device id
 * @param [in]  : u32 excep_id                  exception id
 * @param [in]  : const excep_time *timestamp   exception time
 * @param [in]  : u32 arg
 * @return      : =1: disallow reentrant
 *                =0: success
 *                <0: failure
 */
STATIC s32 rdr_register_system_error(u32 devid, u32 excepid, const struct excep_time *tm, u32 arg)
{
    s32 ret;
    s32 exist = BBOX_FALSE;
    lock_flag_t flags = 0;
    struct rdr_syserr_param_s *node = NULL;
    const struct bbox_exception_info *p_exce_info = NULL;
    struct list_head *cur = NULL;
    struct list_head *next = NULL;
    const struct rdr_syserr_param_s *e_cur = NULL;

    ret = rdr_dfx_add_exception(devid, excepid, tm, arg);
    if (ret != BBOX_SUCCESS) {
        BB_PRINT_INFO("Not process exception. (excepid=0x%x; ret=%d)\n", excepid, ret);
        return BBOX_FAILURE;
    }

    p_exce_info = rdr_exception_get_info(excepid);
    if (p_exce_info == NULL) {
        BB_PRINT_INFO("Can not get exception info. (excepid=0x%x)\n", excepid);
        return BBOX_FAILURE;
    }

    node = (struct rdr_syserr_param_s *)bbox_kmalloc(sizeof(struct rdr_syserr_param_s));
    if (node == NULL) {
        rdr_dfx_del_exception(excepid);
        BB_PRINT_ERR("malloc exception node failed.\n");
        return BBOX_FAILURE;
    }

    RDR_SYSERR_SET_PARAM(*node, excepid, devid, arg, tm->tv_sec, tm->tv_usec * KILO);
    spin_lock_irqsave(&g_rdr_syserr_list_lock, flags);
    if (p_exce_info->e_reentrant == (u32)BBOX_REENTRANT_DISALLOW) {
        list_for_each_safe(cur, next, &g_rdr_syserr_list) {
            e_cur = (const struct rdr_syserr_param_s *)list_entry(cur, struct rdr_syserr_param_s, syserr_list);
            if (e_cur->excepid == node->excepid) {
                exist = BBOX_TRUE;
                break;
            }
        }
    }
    if (exist == BBOX_FALSE) {
        list_add_tail(&node->syserr_list, &g_rdr_syserr_list);
    }
    spin_unlock_irqrestore(&g_rdr_syserr_list_lock, flags);

    // prevent cross-interlocking, intentionally left out for handling
    if (exist == BBOX_TRUE) {
        BB_PRINT_INFO("exception:[0x%x] disallow reentrant. return.\n", excepid);
        rdr_dfx_del_exception(excepid);
        bbox_kfree(node);
        return BBOX_DISALLOW_REENTRANT;
    }
    return BBOX_SUCCESS;
}

STATIC s32 rdr_register_reset(u32 devid, u32 excepid, const struct excep_time *tm, u32 arg)
{
    s32 ret;
    lock_flag_t flags = 0;
    struct rdr_syserr_param_s *node = NULL;

    ret = rdr_dfx_add_exception(devid, excepid, tm, arg);
    if (ret != BBOX_SUCCESS) {
#ifndef BBOX_UT
        BB_PRINT_INFO("Check reset arg, not process exception. (excepid=0x%x; ret=%d)\n", excepid, ret);
#endif
        return BBOX_FAILURE;
    }

    node = (struct rdr_syserr_param_s *)bbox_kmalloc(sizeof(struct rdr_syserr_param_s));
    if (node == NULL) {
        rdr_dfx_del_exception(excepid);
        BB_PRINT_ERR("malloc reset node failed.\n");
        return BBOX_FAILURE;
    }

    RDR_SYSERR_SET_PARAM(*node, excepid, devid, arg, tm->tv_sec, tm->tv_usec * KILO);
    spin_lock_irqsave(&g_rdr_syserr_list_lock, flags);
    list_add_tail(&node->syserr_list, &g_rdr_syserr_list);
    spin_unlock_irqrestore(&g_rdr_syserr_list_lock, flags);
    return BBOX_SUCCESS;
}

s32 rdr_exception_report(u32 devid, u32 excepid, const struct excep_time *tm, u32 arg)
{
    if (bbox_excepid_check_excep_class(excepid) == true) {
        return rdr_register_system_error(devid, excepid, tm, arg);
    } else {
        return rdr_register_reset(devid, excepid, tm, arg);
    }
}

/*
 * @brief       : Latest report exception interface
 * @param [in]  : struct bbox_report_info *info  report info
 * @return      : =1: disallow reentrant
 *                =0: success
 *                <0: failure
 */
s32 bbox_exception_report(const struct bbox_report_info *info)
{
    s32 ret;
    BB_CHECK_PTR(info, return BBOX_FAILURE, "invalid param, report info is NULL.\n");
    BB_CHECK_EXP_ACT((bbox_excepid_check(info->excepid) == false),
                     return BBOX_FAILURE, "invalid param, excepid: 0x%x.\n", info->excepid);
    BB_CHECK_EXP_ACT((info->devid >= bbox_get_device_num()),
                     return BBOX_FAILURE, "invalid param, devid: %u", info->devid);

    BB_PRINT_INFO("[device-%u] blackbox receive [%s] exception at %llu.%llu, exception id is [0x%x], "
                  "status: atomic[%u], irqs disabled[%u], irq[%lu].\n",
                  info->devid, rdr_get_model_name(info->excepid),
                  info->time.tv_sec, info->time.tv_usec, info->excepid,
                  in_atomic(), irqs_disabled(), in_hardirq());

    if (g_init_done == false) {
        BB_PRINT_ERR("bbox rdr hasn't been inited!\n");
        return BBOX_FAILURE;
    }

    ret = rdr_exception_report(info->devid, info->excepid, &info->time, info->arg);
    up(&g_rdr_sem);
    return ret;
}
EXPORT_SYMBOL(bbox_exception_report);

struct bbox_exception_info g_lastword_einfo[] = {
    {   (u32)0xA8021004U, (u32)0xA8021004U,
        BBOX_COREID_MASK(BBOX_DRIVER), BBOX_COREID_MASK(BBOX_DRIVER),
        (u8)DRIVER_EXCEPTION, (u8)BBOX_DRIVER, (u8)BBOX_MAJOR, (u8)BBOX_REBOOT_NO,
        (u8)BBOX_REENTRANT_DISALLOW, "UFS", "UFS Exception", NULL
    },
};

/*
 * @brief       : get exceptioninfo by exceptionid
 * @param [in]  : u32 exceptionid   exception id
 * @return      : einfo: exceptioninfo
 *                NULL: failure
 */
STATIC struct bbox_exception_info *bbox_get_lastword_einfo(u32 exceptionid)
{
    u32 i;
    for (i = 0; i < (sizeof(g_lastword_einfo) / sizeof(struct bbox_exception_info)); i++) {
        if (g_lastword_einfo[i].e_excepid == exceptionid) {
            return &g_lastword_einfo[i];
        }
    }
    return NULL;
}

/*
 * @brief       : set lastword report flag
 * @param [in]  : u32 exceptionid exception id
 * @return      : =0: success
 *                <0: failure
 */
s32 bbox_set_lastword_report_flag(u32 exceptionid)
{
    struct rdr_struct_s *pbb_tmp = NULL;
    struct bbox_time tm = {0, 0};
    const struct bbox_exception_info *einfo = bbox_get_lastword_einfo(exceptionid);
    BB_CHECK_PTR(einfo, return BBOX_FAILURE, "Exception info is NULL, excption id is invalid.\n");
    pbb_tmp =  rdr_field_get_pbb();
    pbb_tmp->top_head.dump_exception = exceptionid;
    bbox_get_safe_systime(&tm);
    rdr_field_baseinfo_reinit();
    rdr_field_save_args(einfo->e_excepid, &tm, 0, 0);
    rdr_field_save_edata(einfo);
    rdr_save_history_log_for_comm(0, einfo, &tm);
    BB_PRINT_INFO("Bbox set lastword report flag sucess. (excepid=0x%x, e_core=%u, e_module=%s, e_desc=%s).\n",
        pbb_tmp->base_info.excepid, pbb_tmp->base_info.e_core, einfo->e_from_module, einfo->e_desc);
    return BBOX_SUCCESS;
}
EXPORT_SYMBOL(bbox_set_lastword_report_flag);

/*
 * @brief       : set os crash
 * @return      : NA
 */
STATIC bool g_rdr_os_crash = false;
STATIC void rdr_set_os_status(void)
{
    g_rdr_os_crash = true;
}

/*
 * @brief       : get os crash
 * @return      : os status
 */
bool rdr_get_os_status(void)
{
    return g_rdr_os_crash;
}

/*
 * @brief       : process the os exception
 * @param [in]  : struct bbox_exception_info *einfo         exception id
 * @param [in]  : struct bbox_time *tm                      exception time
 * @return      : NA
 */
void rdr_syserr_process_for_os(const struct bbox_exception_info *einfo, const struct bbox_time *tm)
{
    BB_CHECK_PTR(einfo, return, "invalid param, einfo is NULL.\n");
    BB_CHECK_PTR(tm, return, "invalid param, tm is NULL.\n");
    rdr_set_os_status();
    rdr_field_baseinfo_reinit();
    rdr_field_save_args(einfo->e_excepid, tm, 0, 0);
    rdr_field_save_edata(einfo);
    rdr_save_history_log_for_panic(0, einfo, tm);
    bbox_record_exce_type(einfo->e_exce_type);
}

#ifdef DEBUG
#define INTER_MS 50
#else
#define INTER_MS 500
#endif

/*
 * @brief       : wait for module dump done
 * @param [in]  : u32 excepid       exception id
 * @param [in]  : u64 mask          modules mask
 * @return      : NA
 */
STATIC void rdr_wait_for_dump_done(u32 excepid, u64 mask)
{
    s32 i = 0;
    s32 wait_dumplog_timeout = (s32)bbox_config_get_dumplog_timeout();
    const s32 wait_time = INTER_MS; // each time sleep 500ms
    u64 cur_mask = 0;

    while (wait_dumplog_timeout > 0) {
        cur_mask = rdr_get_dump_result(excepid);
        if (mask != cur_mask) {
            msleep(wait_time);
            wait_dumplog_timeout -= wait_time;
            i++;
        } else {
            BB_PRINT_INFO("dump done. use time:[%d], cur_mask[0x%llx]\n", i * wait_time, cur_mask);
            break;
        }
    }

    if (wait_dumplog_timeout <= 0) {
        BB_PRINT_INFO("wait for dump status timeout... cur_mask[0x%llx], target_mask[0x%llx]\n", cur_mask, mask);
    }
}

/*
 * @brief       : wait for suspend
 * @return      : NA
 */
STATIC void rdr_wait_for_suspend(void)
{
    while (1) {
        if (rdr_get_suspend_state() != 0) {
            msleep(WAIT_MS_VSHORT); // each time sleep 50ms
        } else {
            break;
        }
    }
}

/*
 * @brief       : process reset exception
 * @param [in]  : struct rdr_syserr_param_s *param      param
 * @return      : NA
 */
STATIC void rdr_syserr_reset(const struct rdr_syserr_param_s *param)
{
    s32 ret;
    u32 excepid;
    const struct bbox_exception_info *p_exce_info = NULL;
    struct bbox_exception_info e_info;

    BB_CHECK_PTR(param, return, "invalid param, p is NULL.\n");

    ret = memset_s(&e_info, sizeof(e_info), 0, sizeof(e_info));
    BB_CHECK_RET(ret != EOK, "%s:%4d e_info memset_s error.\n", __func__, __LINE__);

    excepid = bbox_excepid_transf_class(param->excepid);
    p_exce_info = rdr_exception_get_info(excepid);
    if (p_exce_info == NULL) {
        BB_PRINT_ERR("get exception info failed. return.\n");
        return;
    }

    // 1. delete from exception reporting chain
    ret = rdr_del_device_errorcode(param->devid, excepid);
    BB_CHECK_RET(ret != BBOX_SUCCESS, "delete device-%u error code[0x%x] failed.\n", param->devid, excepid);

    // 2. write log
    ret = memcpy_s(&e_info, sizeof(e_info), p_exce_info, sizeof(struct bbox_exception_info));
    BB_CHECK_MEMCPY(ret, return);

    e_info.e_excepid = param->excepid;
    e_info.e_excepid_end = param->excepid;
    rdr_save_history_log_for_runtime(param->devid, &e_info, &(param->tm));

    // 3. send to remote
    ret = rdr_save_dump_for_reset(param->devid, param->excepid, p_exce_info->e_from_core, &(param->tm));
    BB_CHECK_RET(ret != BBOX_SUCCESS, "send device-%u reset code[0x%x] failed.\n", param->devid, param->excepid);
    return;
}

/*
 * @brief       : process exception
 * @param [in]  : struct rdr_syserr_param_s *param      param
 * @return      : NA
 */
STATIC void rdr_syserr_process(struct rdr_syserr_param_s *param)
{
    u32 excepid;
    u64 mask;
    struct bbox_exception_info *p_exce_info = NULL;

    BB_CHECK_PTR(param, return, "invalid param, param is NULL.\n");

    excepid = param->excepid;
    rdr_field_baseinfo_reinit();
    bbox_update_time_seq(&(param->tm));
    rdr_field_save_args(param->excepid, &param->tm, param->devid, param->arg);
    p_exce_info = rdr_exception_get_info(excepid);

    rdr_wait_for_suspend();

    if (p_exce_info == NULL) {
        BB_PRINT_ERR("get exception info failed. return.\n");
        return;
    }

    BB_PRINT_INFO("start saving data.\n");
    rdr_set_saving_state(BBOX_TRUE);

    rdr_exception_print_info(p_exce_info);
    rdr_field_save_edata(p_exce_info);

    rdr_save_history_log_for_runtime(param->devid, p_exce_info, &(param->tm));

    mask = rdr_notify_module_dump(param->devid, excepid, (const struct bbox_time *)&param->tm, param->arg, p_exce_info);
    if (mask != 0) {
        // The value of mask is obtained based on p_exce_info->e_notify_core_mask,
        // When mask is set to 0, the log export process of the RDR framework is not used.
        rdr_wait_for_dump_done(excepid, mask);
    }

    rdr_field_procexec_done();

    rdr_set_saving_state(BBOX_FALSE);
    rdr_exception_callback(p_exce_info, excepid);

    if (bbox_excepid_check_reset_class(excepid) != true) {
        s32 ret = rdr_add_device_errorcode(param->devid, excepid, p_exce_info);
        BB_CHECK_RET(ret != BBOX_SUCCESS, "Add device error code to excepid list failed.\n");
    }

    rdr_notify_module_reset(param->devid, excepid, p_exce_info);

    return;
}

/*
 * @brief       : list is empty
 * @return      : true: yes; false: no
 */
bool rdr_syserr_list_empty(void)
{
    return ((list_empty(&g_rdr_syserr_list) != 0) ? true : false);
}

STATIC void rdr_process_syserr(void)
{
    u32 e_priority = BBOX_PPRI_MAX;
    lock_flag_t flags = 0;
    struct list_head *cur = NULL;
    struct list_head *next = NULL;
    struct list_head *process = NULL;
    struct rdr_syserr_param_s *e_cur = NULL;
    struct rdr_syserr_param_s *e_process = NULL;
    const struct bbox_exception_info *p_exce_info = NULL;

    while (!rdr_syserr_list_empty()) {
        spin_lock_irqsave(&g_rdr_syserr_list_lock, flags);
        list_for_each_safe(cur, next, &g_rdr_syserr_list) {
            e_cur = (struct rdr_syserr_param_s *)list_entry(cur, struct rdr_syserr_param_s, syserr_list);
            p_exce_info = rdr_exception_get_info(e_cur->excepid);
            if ((p_exce_info == NULL) || (p_exce_info->e_process_priority >= (u8)BBOX_PPRI_MAX)) {
                list_del(cur);
                BBOX_KFREE(e_cur);
                continue;
            }
            // Find the one with the highest processing priority among all received exceptions in the linked list.
            if ((p_exce_info->e_process_priority > e_priority) || (e_priority == (u32)BBOX_PPRI_MAX)) {
                process = cur;
                e_process = e_cur;
                e_priority = p_exce_info->e_process_priority;
            }
        }

        if ((process == NULL) || (e_process == NULL)) {
            spin_unlock_irqrestore(&g_rdr_syserr_list_lock, flags);
            BB_PRINT_INFO("exception: NULL\n");
            continue;
        }

        list_del(process);
        spin_unlock_irqrestore(&g_rdr_syserr_list_lock, flags);

        bbox_excepid_check_excep_class(e_process->excepid) ?
            rdr_syserr_process(e_process) : rdr_syserr_reset(e_process);
        rdr_dfx_del_exception(e_process->excepid);

        BBOX_KFREE(e_process);
        e_priority = BBOX_PPRI_MAX;
        process = NULL;
    }
}

/*
 * @brief       : main thread process
 * @param [in]  : void *arg     arg
 * @return      : <0 failure; =0 success
 */
STATIC s32 rdr_main_thread_body(void *arg)
{
    BB_PRINT_INFO("rdr main thread running.\n");
    UNUSED(arg);

    while (!kthread_should_stop()) {
        bbox_jiffies_t rdr_jiffies = msecs_to_jiffies(WAIT_MS_LONG);
        if ((down_timeout(&g_rdr_sem, (bbox_long_t)rdr_jiffies) != 0) && rdr_syserr_list_empty()) {
            continue;
        }
        rdr_process_syserr();
    }
    return BBOX_SUCCESS;
}

/*
 * @brief       : rdr init
 * @return      : true: init done, false: inti failed
 */
bool rdr_init_done(void)
{
    return g_init_done;
}

/*
 * @brief       : rdr deinitialize
 * @return      : NA
 */
static inline void rdr_deinitialize(void)
{
    g_rdr_os_crash = false;
    sema_init(&g_rdr_sem, 0);
    rdr_module_exit();
    rdr_exception_exit();
    rdr_dfx_exit();
    rdr_field_exit();
    rdr_common_exit();
}

/*
 * @brief       : module init function, startup threads
 * @return      : <0 failure; =0 success
 */
STATIC struct task_struct *g_rdr_main = NULL;
s32 bbox_rdr_init(void)
{
    if (rdr_init_done()) {
        return BBOX_SUCCESS;
    }

    if (rdr_common_init() != BBOX_SUCCESS) {
        BB_PRINT_ERR("rdr_common_init failed.\n");
        return BBOX_FAILURE;
    }

    if (rdr_field_init() != BBOX_SUCCESS) {
        BB_PRINT_ERR("rdr_field_init failed.\n");
        rdr_common_exit();
        return BBOX_FAILURE;
    }

    (void)rdr_module_init();
    (void)rdr_exception_init();
    (void)rdr_dfx_init();

    sema_init(&g_rdr_sem, 0);
    g_rdr_main = bbox_kthread_proc(rdr_main_thread_body, NULL, "bbox_main");
    if (g_rdr_main == NULL) {
        BB_PRINT_ERR("create thread rdr_main_thread failed.\n");
        rdr_deinitialize();
        return BBOX_FAILURE;
    }

    BB_PRINT_INFO("rdr initialized.\n");
    g_init_done = true;
    return BBOX_SUCCESS;
}

/*
 * @brief       : module exit function
 * @return      : NA
 */
void bbox_rdr_exit(void)
{
    if (g_init_done == false) {
        return;
    }
    KTHREAD_STOP(g_rdr_main);
    BB_PRINT_INFO("close rdr main thread.\n");

    rdr_deinitialize();
    g_init_done = false;
}
