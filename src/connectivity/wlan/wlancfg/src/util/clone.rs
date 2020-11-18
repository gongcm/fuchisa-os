// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_wlan_internal as fidl_internal;
use fidl_fuchsia_wlan_sme as fidl_sme;

fn clone_bss_description(desc: &fidl_internal::BssDescription) -> fidl_internal::BssDescription {
    fidl_internal::BssDescription {
        ssid: desc.ssid.clone(),
        rates: desc.rates.clone(),
        country: desc.country.clone(),
        rsne: desc.rsne.clone(),
        vendor_ies: desc.vendor_ies.clone(),
        ht_cap: desc.ht_cap.clone(),
        ht_op: desc.ht_op.clone(),
        vht_cap: desc.vht_cap.clone(),
        vht_op: desc.vht_op.clone(),
        ..*desc
    }
}

pub(crate) fn clone_bss_info(bss: &fidl_sme::BssInfo) -> fidl_sme::BssInfo {
    fidl_sme::BssInfo {
        bssid: bss.bssid.clone(),
        ssid: bss.ssid.clone(),
        rssi_dbm: bss.rssi_dbm,
        snr_db: bss.snr_db,
        channel: bss.channel,
        protection: bss.protection,
        compatible: bss.compatible,
        bss_desc: bss.bss_desc.as_ref().map(|desc| Box::new(clone_bss_description(&desc))),
    }
}
