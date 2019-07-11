/*
 * Copyright (c) 2013 Broadcom Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "btcoex.h"

#include "brcmu_utils.h"
#include "brcmu_wifi.h"
#include "cfg80211.h"
#include "core.h"
#include "debug.h"
#include "defs.h"
#include "device.h"
#include "fwil.h"
#include "fwil_types.h"
#include "linuxisms.h"
#include "p2p.h"
#include "workqueue.h"

/* T1 start SCO/eSCO priority suppression */
#define BRCMF_BTCOEX_OPPR_WIN_TIME_MSEC (2000)

/* BT registers values during DHCP */
#define BRCMF_BT_DHCP_REG50 0x8022
#define BRCMF_BT_DHCP_REG51 0
#define BRCMF_BT_DHCP_REG64 0
#define BRCMF_BT_DHCP_REG65 0
#define BRCMF_BT_DHCP_REG71 0
#define BRCMF_BT_DHCP_REG66 0x2710
#define BRCMF_BT_DHCP_REG41 0x33
#define BRCMF_BT_DHCP_REG68 0x190

/* number of samples for SCO detection */
#define BRCMF_BT_SCO_SAMPLES 12

/**
 * enum brcmf_btcoex_state - BT coex DHCP state machine states
 * @BRCMF_BT_DHCP_IDLE: DCHP is idle
 * @BRCMF_BT_DHCP_START: DHCP started, wait before
 *  boosting wifi priority
 * @BRCMF_BT_DHCP_OPPR_WIN: graceful DHCP opportunity ended,
 *  boost wifi priority
 * @BRCMF_BT_DHCP_FLAG_FORCE_TIMEOUT: wifi priority boost end,
 *  restore defaults
 */
enum brcmf_btcoex_state {
    BRCMF_BT_DHCP_IDLE,
    BRCMF_BT_DHCP_START,
    BRCMF_BT_DHCP_OPPR_WIN,
    BRCMF_BT_DHCP_FLAG_FORCE_TIMEOUT
};

/**
 * struct brcmf_btcoex_info - BT coex related information
 * @vif: interface for which request was done.
 * @timer: timer for DHCP state machine
 * @timeout: configured timeout.
 * @timer_on:  DHCP timer active
 * @dhcp_done: DHCP finished before T1/T2 timer expiration
 * @bt_state: DHCP state machine state
 * @work: DHCP state machine work
 * @cfg: driver private data for cfg80211 interface
 * @reg66: saved value of btc_params 66
 * @reg41: saved value of btc_params 41
 * @reg68: saved value of btc_params 68
 * @saved_regs_part1: flag indicating regs 66,41,68
 *  have been saved
 * @reg51: saved value of btc_params 51
 * @reg64: saved value of btc_params 64
 * @reg65: saved value of btc_params 65
 * @reg71: saved value of btc_params 71
 * @saved_regs_part1: flag indicating regs 50,51,64,65,71
 *  have been saved
 */
struct brcmf_btcoex_info {
    struct brcmf_cfg80211_vif* vif;
    brcmf_timer_info_t timer;
    uint16_t timeout;
    bool timer_on;
    bool dhcp_done;
    enum brcmf_btcoex_state bt_state;
    struct work_struct work;
    struct brcmf_cfg80211_info* cfg;
    uint32_t reg66;
    uint32_t reg41;
    uint32_t reg68;
    bool saved_regs_part1;
    uint32_t reg50;
    uint32_t reg51;
    uint32_t reg64;
    uint32_t reg65;
    uint32_t reg71;
    bool saved_regs_part2;
};

/**
 * brcmf_btcoex_params_write() - write btc_params firmware variable
 * @ifp: interface
 * @addr: btc_params register number
 * @data: data to write
 */
static zx_status_t brcmf_btcoex_params_write(struct brcmf_if* ifp, uint32_t addr, uint32_t data) {
    struct {
        uint32_t addr;
        uint32_t data;
    } reg_write;

    reg_write.addr = addr;
    reg_write.data = data;
    return brcmf_fil_iovar_data_set(ifp, "btc_params", &reg_write, sizeof(reg_write), nullptr);
}

/**
 * brcmf_btcoex_params_read() - read btc_params firmware variable
 * @ifp: interface
 * @addr: btc_params register number
 * @data: read data
 */
static zx_status_t brcmf_btcoex_params_read(struct brcmf_if* ifp, uint32_t addr, uint32_t* data) {
    *data = addr;

    return brcmf_fil_iovar_int_get(ifp, "btc_params", data, nullptr);
}

/**
 * brcmf_btcoex_boost_wifi() - control BT SCO/eSCO parameters
 * @btci: BT coex info
 * @trump_sco:
 *  true - set SCO/eSCO parameters for compatibility
 *      during DHCP window
 *  false - restore saved parameter values
 *
 * Enhanced BT COEX settings for eSCO compatibility during DHCP window
 */
static void brcmf_btcoex_boost_wifi(struct brcmf_btcoex_info* btci, bool trump_sco) {
    struct brcmf_if* ifp = brcmf_get_ifp(btci->cfg->pub, 0);

    if (trump_sco && !btci->saved_regs_part2) {
        /* this should reduce eSCO agressive
         * retransmit w/o breaking it
         */

        /* save current */
        BRCMF_DBG(INFO, "new SCO/eSCO coex algo {save & override}\n");
        brcmf_btcoex_params_read(ifp, 50, &btci->reg50);
        brcmf_btcoex_params_read(ifp, 51, &btci->reg51);
        brcmf_btcoex_params_read(ifp, 64, &btci->reg64);
        brcmf_btcoex_params_read(ifp, 65, &btci->reg65);
        brcmf_btcoex_params_read(ifp, 71, &btci->reg71);

        btci->saved_regs_part2 = true;
        BRCMF_DBG(INFO, "saved bt_params[50,51,64,65,71]: 0x%x 0x%x 0x%x 0x%x 0x%x\n", btci->reg50,
                  btci->reg51, btci->reg64, btci->reg65, btci->reg71);

        /* pacify the eSco   */
        brcmf_btcoex_params_write(ifp, 50, BRCMF_BT_DHCP_REG50);
        brcmf_btcoex_params_write(ifp, 51, BRCMF_BT_DHCP_REG51);
        brcmf_btcoex_params_write(ifp, 64, BRCMF_BT_DHCP_REG64);
        brcmf_btcoex_params_write(ifp, 65, BRCMF_BT_DHCP_REG65);
        brcmf_btcoex_params_write(ifp, 71, BRCMF_BT_DHCP_REG71);

    } else if (btci->saved_regs_part2) {
        /* restore previously saved bt params */
        BRCMF_DBG(INFO, "Do new SCO/eSCO coex algo {restore}\n");
        brcmf_btcoex_params_write(ifp, 50, btci->reg50);
        brcmf_btcoex_params_write(ifp, 51, btci->reg51);
        brcmf_btcoex_params_write(ifp, 64, btci->reg64);
        brcmf_btcoex_params_write(ifp, 65, btci->reg65);
        brcmf_btcoex_params_write(ifp, 71, btci->reg71);

        BRCMF_DBG(INFO, "restored bt_params[50,51,64,65,71]: 0x%x 0x%x 0x%x 0x%x 0x%x\n",
                  btci->reg50, btci->reg51, btci->reg64, btci->reg65, btci->reg71);

        btci->saved_regs_part2 = false;
    } else {
        BRCMF_DBG(INFO, "attempted to restore not saved BTCOEX params\n");
    }
}

/**
 * brcmf_btcoex_is_sco_active() - check if SCO/eSCO is active
 * @ifp: interface
 *
 * return: true if SCO/eSCO session is active
 */
static bool brcmf_btcoex_is_sco_active(struct brcmf_if* ifp) {
    int ioc_res = 0;
    bool res = false;
    int sco_id_cnt = 0;
    uint32_t param27;
    int i;

    for (i = 0; i < BRCMF_BT_SCO_SAMPLES; i++) {
        ioc_res = brcmf_btcoex_params_read(ifp, 27, &param27);

        if (ioc_res < 0) {
            BRCMF_ERR("ioc read btc params error\n");
            break;
        }

        BRCMF_DBG(INFO, "sample[%d], btc_params 27:%x\n", i, param27);

        if ((param27 & 0x6) == 2) { /* count both sco & esco  */
            sco_id_cnt++;
        }

        if (sco_id_cnt > 2) {
            BRCMF_DBG(INFO, "sco/esco detected, pkt id_cnt:%d samples:%d\n", sco_id_cnt, i);
            res = true;
            break;
        }
    }
    BRCMF_DBG(TRACE, "exit: result=%d\n", res);
    return res;
}

/**
 * btcmf_btcoex_save_part1() - save first step parameters.
 */
static void btcmf_btcoex_save_part1(struct brcmf_btcoex_info* btci) {
    struct brcmf_if* ifp = btci->vif->ifp;

    if (!btci->saved_regs_part1) {
        /* Retrieve and save original reg value */
        brcmf_btcoex_params_read(ifp, 66, &btci->reg66);
        brcmf_btcoex_params_read(ifp, 41, &btci->reg41);
        brcmf_btcoex_params_read(ifp, 68, &btci->reg68);
        btci->saved_regs_part1 = true;
        BRCMF_DBG(INFO, "saved btc_params regs (66,41,68) 0x%x 0x%x 0x%x\n", btci->reg66,
                  btci->reg41, btci->reg68);
    }
}

/**
 * brcmf_btcoex_restore_part1() - restore first step parameters.
 */
static void brcmf_btcoex_restore_part1(struct brcmf_btcoex_info* btci) {
    struct brcmf_if* ifp;

    if (btci->saved_regs_part1) {
        btci->saved_regs_part1 = false;
        ifp = btci->vif->ifp;
        brcmf_btcoex_params_write(ifp, 66, btci->reg66);
        brcmf_btcoex_params_write(ifp, 41, btci->reg41);
        brcmf_btcoex_params_write(ifp, 68, btci->reg68);
        BRCMF_DBG(INFO, "restored btc_params regs {66,41,68} 0x%x 0x%x 0x%x\n", btci->reg66,
                  btci->reg41, btci->reg68);
    }
}

/**
 * brcmf_btcoex_timerfunc() - BT coex timer callback
 */
static void brcmf_btcoex_timerfunc(void* data) {
    pthread_mutex_lock(&irq_callback_lock);
    struct brcmf_btcoex_info* bt_local = static_cast<decltype(bt_local)>(data);
    BRCMF_DBG(TRACE, "enter\n");

    bt_local->timer_on = false;
    workqueue_schedule_default(&bt_local->work);
    pthread_mutex_unlock(&irq_callback_lock);
}

/**
 * brcmf_btcoex_handler() - BT coex state machine work handler
 * @work: work
 */
static void brcmf_btcoex_handler(struct work_struct* work) {
    struct brcmf_btcoex_info* btci;
    btci = containerof(work, struct brcmf_btcoex_info, work);
    if (btci->timer_on) {
        btci->timer_on = false;
        brcmf_timer_stop(&btci->timer);
    }

    switch (btci->bt_state) {
    case BRCMF_BT_DHCP_START:
        /* DHCP started provide OPPORTUNITY window
           to get DHCP address
        */
        BRCMF_DBG(INFO, "DHCP started\n");
        btci->bt_state = BRCMF_BT_DHCP_OPPR_WIN;
        if (btci->timeout < BRCMF_BTCOEX_OPPR_WIN_TIME_MSEC) {
            // TODO(cphoenix): Was btci->timer.expires which wasn't set anywhere
            brcmf_timer_set(&btci->timer, btci->timeout);
        } else {
            btci->timeout -= BRCMF_BTCOEX_OPPR_WIN_TIME_MSEC;
            brcmf_timer_set(&btci->timer, ZX_MSEC(BRCMF_BTCOEX_OPPR_WIN_TIME_MSEC));
        }
        btci->timer_on = true;
        break;

    case BRCMF_BT_DHCP_OPPR_WIN:
        if (btci->dhcp_done) {
            BRCMF_DBG(INFO, "DHCP done before T1 expiration\n");
            goto idle;
        }

        /* DHCP is not over yet, start lowering BT priority */
        BRCMF_DBG(INFO, "DHCP T1:%d expired\n", BRCMF_BTCOEX_OPPR_WIN_TIME_MSEC);
        brcmf_btcoex_boost_wifi(btci, true);

        btci->bt_state = BRCMF_BT_DHCP_FLAG_FORCE_TIMEOUT;
        brcmf_timer_set(&btci->timer, ZX_MSEC(btci->timeout));
        btci->timer_on = true;
        break;

    case BRCMF_BT_DHCP_FLAG_FORCE_TIMEOUT:
        if (btci->dhcp_done) {
            BRCMF_DBG(INFO, "DHCP done before T2 expiration\n");
        } else {
            BRCMF_DBG(INFO, "DHCP T2:%d expired\n", BRCMF_BT_DHCP_FLAG_FORCE_TIMEOUT);
        }

        goto idle;

    default:
        BRCMF_ERR("invalid state=%d !!!\n", btci->bt_state);
        goto idle;
    }

    return;

idle:
    btci->bt_state = BRCMF_BT_DHCP_IDLE;
    btci->timer_on = false;
    brcmf_btcoex_boost_wifi(btci, false);
    cfg80211_crit_proto_stopped(&btci->vif->wdev);
    brcmf_btcoex_restore_part1(btci);
    btci->vif = NULL;
}

/**
 * brcmf_btcoex_attach() - initialize BT coex data
 * @cfg: driver private cfg80211 data
 *
 * return: 0 on success
 */
zx_status_t brcmf_btcoex_attach(struct brcmf_cfg80211_info* cfg) {
    struct brcmf_btcoex_info* btci = NULL;
    BRCMF_DBG(TRACE, "enter\n");

    btci = static_cast<decltype(btci)>(malloc(sizeof(struct brcmf_btcoex_info)));
    if (!btci) {
        return ZX_ERR_NO_MEMORY;
    }

    btci->bt_state = BRCMF_BT_DHCP_IDLE;

    /* Set up timer for BT  */
    btci->timer_on = false;
    btci->timeout = BRCMF_BTCOEX_OPPR_WIN_TIME_MSEC;
    brcmf_timer_init(&btci->timer, brcmf_btcoex_timerfunc, btci);
    btci->cfg = cfg;
    btci->saved_regs_part1 = false;
    btci->saved_regs_part2 = false;

    workqueue_init_work(&btci->work, brcmf_btcoex_handler);

    cfg->btcoex = btci;
    return ZX_OK;
}

/**
 * brcmf_btcoex_detach - clean BT coex data
 * @cfg: driver private cfg80211 data
 */
void brcmf_btcoex_detach(struct brcmf_cfg80211_info* cfg) {
    BRCMF_DBG(TRACE, "enter\n");

    if (!cfg->btcoex) {
        return;
    }

    if (cfg->btcoex->timer_on) {
        cfg->btcoex->timer_on = false;
        brcmf_timer_stop(&cfg->btcoex->timer);
    }

    workqueue_cancel_work(&cfg->btcoex->work);

    brcmf_btcoex_boost_wifi(cfg->btcoex, false);
    brcmf_btcoex_restore_part1(cfg->btcoex);

    free(cfg->btcoex);
    cfg->btcoex = NULL;
}

static void brcmf_btcoex_dhcp_start(struct brcmf_btcoex_info* btci) {
    struct brcmf_if* ifp = btci->vif->ifp;

    btcmf_btcoex_save_part1(btci);
    /* set new regs values */
    brcmf_btcoex_params_write(ifp, 66, BRCMF_BT_DHCP_REG66);
    brcmf_btcoex_params_write(ifp, 41, BRCMF_BT_DHCP_REG41);
    brcmf_btcoex_params_write(ifp, 68, BRCMF_BT_DHCP_REG68);
    btci->dhcp_done = false;
    btci->bt_state = BRCMF_BT_DHCP_START;
    workqueue_schedule_default(&btci->work);
    BRCMF_DBG(TRACE, "enable BT DHCP Timer\n");
}

static void brcmf_btcoex_dhcp_end(struct brcmf_btcoex_info* btci) {
    /* Stop any bt timer because DHCP session is done */
    btci->dhcp_done = true;
    if (btci->timer_on) {
        BRCMF_DBG(INFO, "disable BT DHCP Timer\n");
        btci->timer_on = false;
        brcmf_timer_stop(&btci->timer);

        /* schedule worker if transition to IDLE is needed */
        if (btci->bt_state != BRCMF_BT_DHCP_IDLE) {
            BRCMF_DBG(INFO, "bt_state:%d\n", btci->bt_state);
            workqueue_schedule_default(&btci->work);
        }
    } else {
        /* Restore original values */
        brcmf_btcoex_restore_part1(btci);
    }
}

/**
 * brcmf_btcoex_set_mode - set BT coex mode
 * @cfg: driver private cfg80211 data
 * @mode: Wifi-Bluetooth coexistence mode
 *
 * return: 0 on success
 */
zx_status_t brcmf_btcoex_set_mode(struct brcmf_cfg80211_vif* vif, enum brcmf_btcoex_mode mode,
                                  uint16_t duration) {
    struct brcmf_cfg80211_info* cfg = wiphy_to_cfg(vif->wdev.wiphy);
    struct brcmf_btcoex_info* btci = cfg->btcoex;
    struct brcmf_if* ifp = brcmf_get_ifp(cfg->pub, 0);

    switch (mode) {
    case BRCMF_BTCOEX_DISABLED:
        BRCMF_DBG(INFO, "DHCP session starts\n");
        if (btci->bt_state != BRCMF_BT_DHCP_IDLE) {
            return ZX_ERR_UNAVAILABLE;
        }
        /* Start BT timer only for SCO connection */
        if (brcmf_btcoex_is_sco_active(ifp)) {
            btci->timeout = duration;
            btci->vif = vif;
            brcmf_btcoex_dhcp_start(btci);
        }
        break;

    case BRCMF_BTCOEX_ENABLED:
        BRCMF_DBG(INFO, "DHCP session ends\n");
        if (btci->bt_state != BRCMF_BT_DHCP_IDLE && vif == btci->vif) {
            brcmf_btcoex_dhcp_end(btci);
        }
        break;
    default:
        BRCMF_DBG(INFO, "Unknown mode, ignored\n");
    }
    return ZX_OK;
}
