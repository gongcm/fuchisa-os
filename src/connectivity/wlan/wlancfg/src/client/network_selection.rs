// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        client::{
            scan::{self, ScanResultUpdate},
            types,
        },
        config_management::{self, Credential, SavedNetworksManager},
        mode_management::iface_manager_api::IfaceManagerApi,
    },
    async_trait::async_trait,
    fidl_fuchsia_wlan_internal as fidl_internal, fidl_fuchsia_wlan_sme as fidl_sme,
    fuchsia_cobalt::CobaltSender,
    fuchsia_zircon as zx,
    futures::lock::Mutex,
    log::{debug, error, info, trace},
    std::{cmp::Ordering, collections::HashMap, convert::TryInto, sync::Arc},
    wlan_common::channel::Channel,
    wlan_metrics_registry::{
        ActiveScanRequestedForNetworkSelectionMetricDimensionActiveScanSsidsRequested as ActiveScanSsidsRequested,
        SavedNetworkInScanResultMetricDimensionBssCount,
        SavedNetworkInScanResultWithActiveScanMetricDimensionActiveScanSsidsObserved as ActiveScanSsidsObserved,
        ScanResultsReceivedMetricDimensionSavedNetworksCount,
        ACTIVE_SCAN_REQUESTED_FOR_NETWORK_SELECTION_METRIC_ID,
        LAST_SCAN_AGE_WHEN_SCAN_REQUESTED_METRIC_ID, SAVED_NETWORK_IN_SCAN_RESULT_METRIC_ID,
        SAVED_NETWORK_IN_SCAN_RESULT_WITH_ACTIVE_SCAN_METRIC_ID, SCAN_RESULTS_RECEIVED_METRIC_ID,
    },
};

const RECENT_FAILURE_WINDOW: zx::Duration = zx::Duration::from_seconds(60 * 5); // 5 minutes
const STALE_SCAN_AGE: zx::Duration = zx::Duration::from_seconds(10); // TODO(61992) Tweak duration
/// Above this RSSI, we'll give 5G networks a preference
const RSSI_CUTOFF_5G_PREFERENCE: i8 = -50;
/// The score boost for 5G networks that we are giving preference to.
const RSSI_5G_PREFERENCE_BOOST: i8 = 15;

pub struct NetworkSelector {
    saved_network_manager: Arc<SavedNetworksManager>,
    scan_result_cache: Arc<Mutex<ScanResultCache>>,
    cobalt_api: Arc<Mutex<CobaltSender>>,
}

struct ScanResultCache {
    updated_at: zx::Time,
    results: Vec<types::ScanResult>,
}

#[derive(Debug, PartialEq, Clone)]
struct InternalSavedNetworkData {
    credential: Credential,
    has_ever_connected: bool,
    recent_failure_count: u8,
}

#[derive(Debug, Clone, PartialEq)]
struct InternalBss<'a> {
    network_id: types::NetworkIdentifier,
    network_info: InternalSavedNetworkData,
    bss_info: &'a types::Bss,
}

impl InternalBss<'_> {
    fn score(&self) -> i8 {
        let rssi = self.bss_info.rssi;
        let channel = Channel::from_fidl(self.bss_info.channel);

        // If the network is 5G and has a strong enough RSSI, give it a bonus
        if channel.is_5ghz() && rssi > RSSI_CUTOFF_5G_PREFERENCE {
            return rssi.saturating_add(RSSI_5G_PREFERENCE_BOOST);
        }
        return rssi;
    }
}

impl NetworkSelector {
    pub fn new(saved_network_manager: Arc<SavedNetworksManager>, cobalt_api: CobaltSender) -> Self {
        Self {
            saved_network_manager,
            scan_result_cache: Arc::new(Mutex::new(ScanResultCache {
                updated_at: zx::Time::ZERO,
                results: Vec::new(),
            })),
            cobalt_api: Arc::new(Mutex::new(cobalt_api)),
        }
    }

    pub fn generate_scan_result_updater(&self) -> NetworkSelectorScanUpdater {
        NetworkSelectorScanUpdater {
            scan_result_cache: Arc::clone(&self.scan_result_cache),
            saved_network_manager: Arc::clone(&self.saved_network_manager),
            cobalt_api: Arc::clone(&self.cobalt_api),
        }
    }

    async fn perform_scan(&self, iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>) {
        // Get the scan age.
        let scan_result_guard = self.scan_result_cache.lock().await;
        let last_scan_result_time = scan_result_guard.updated_at;
        drop(scan_result_guard);
        let scan_age = zx::Time::get_monotonic() - last_scan_result_time;

        // Log a metric for scan age, to help us optimize the STALE_SCAN_AGE
        if last_scan_result_time != zx::Time::ZERO {
            let mut cobalt_api_guard = self.cobalt_api.lock().await;
            let cobalt_api = &mut *cobalt_api_guard;
            cobalt_api.log_elapsed_time(
                LAST_SCAN_AGE_WHEN_SCAN_REQUESTED_METRIC_ID,
                Vec::<u32>::new(),
                scan_age.into_micros(),
            );
            drop(cobalt_api_guard);
        }

        // Determine if a new scan is warranted
        if scan_age >= STALE_SCAN_AGE {
            if last_scan_result_time != zx::Time::ZERO {
                info!("Scan results are {}s old, triggering a scan", scan_age.into_seconds());
            }

            let mut cobalt_api_clone = self.cobalt_api.lock().await.clone();
            let potentially_hidden_saved_networks =
                config_management::select_subset_potentially_hidden_networks(
                    self.saved_network_manager.get_networks().await,
                );

            scan::perform_scan(
                iface_manager,
                None,
                self.generate_scan_result_updater(),
                scan::LocationSensorUpdater {},
                |_| {
                    let active_scan_request_count_metric =
                        match potentially_hidden_saved_networks.len() {
                            0 => ActiveScanSsidsRequested::Zero,
                            1 => ActiveScanSsidsRequested::One,
                            2..=4 => ActiveScanSsidsRequested::TwoToFour,
                            5..=10 => ActiveScanSsidsRequested::FiveToTen,
                            11..=20 => ActiveScanSsidsRequested::ElevenToTwenty,
                            21..=50 => ActiveScanSsidsRequested::TwentyOneToFifty,
                            51..=100 => ActiveScanSsidsRequested::FiftyOneToOneHundred,
                            101..=usize::MAX => ActiveScanSsidsRequested::OneHundredAndOneOrMore,
                            _ => unreachable!(),
                        };
                    cobalt_api_clone.log_event(
                        ACTIVE_SCAN_REQUESTED_FOR_NETWORK_SELECTION_METRIC_ID,
                        active_scan_request_count_metric,
                    );

                    if potentially_hidden_saved_networks.is_empty() {
                        None
                    } else {
                        Some(potentially_hidden_saved_networks)
                    }
                },
            )
            .await;
        } else {
            info!("Using cached scan results from {}s ago", scan_age.into_seconds());
        }
    }

    /// Select the best available network, based on the current saved networks and the most
    /// recent scan results provided to this module.
    /// Only networks that are both saved and visible in the most recent scan results are eligible
    /// for consideration. Among those, the "best" network based on compatibility and quality (e.g.
    /// RSSI, recent failures) is selected.
    pub(crate) async fn find_best_connection_candidate(
        &self,
        iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
        ignore_list: &Vec<types::NetworkIdentifier>,
    ) -> Option<types::ConnectionCandidate> {
        self.perform_scan(iface_manager.clone()).await;
        let saved_networks = load_saved_networks(Arc::clone(&self.saved_network_manager)).await;
        let scan_result_guard = self.scan_result_cache.lock().await;
        let networks =
            merge_saved_networks_and_scan_data(saved_networks, &scan_result_guard.results).await;

        match select_best_connection_candidate(networks, ignore_list) {
            Some((selected, channel, bssid)) => {
                Some(augment_bss_with_active_scan(selected, channel, bssid, iface_manager).await)
            }
            None => None,
        }
    }

    /// Find a suitable BSS for the given network.
    pub(crate) async fn find_connection_candidate_for_network(
        &self,
        sme_proxy: fidl_sme::ClientSmeProxy,
        network: types::NetworkIdentifier,
    ) -> Option<types::ConnectionCandidate> {
        // TODO: check if we have recent enough scan results that we can pull from instead?
        let scan_results =
            scan::perform_directed_active_scan(&sme_proxy, &network.ssid, None).await;

        match scan_results {
            Err(()) => None,
            Ok(scan_results) => {
                let saved_networks =
                    load_saved_networks(Arc::clone(&self.saved_network_manager)).await;
                let networks =
                    merge_saved_networks_and_scan_data(saved_networks, &scan_results).await;
                let ignore_list = vec![];
                select_best_connection_candidate(networks, &ignore_list).map(|(candidate, _, _)| {
                    // Strip out the information about passive vs active scan, because we can't know
                    // if this network would have been observed in a passive scan (since we never
                    // performed a passive scan).
                    types::ConnectionCandidate { observed_in_passive_scan: None, ..candidate }
                })
            }
        }
    }
}

/// Merge the saved networks and scan results into a vector of BSSs that correspond to a saved
/// network.
async fn merge_saved_networks_and_scan_data<'a>(
    saved_networks: HashMap<types::NetworkIdentifier, InternalSavedNetworkData>,
    scan_results: &'a Vec<types::ScanResult>,
) -> Vec<InternalBss<'a>> {
    let mut merged_networks = vec![];
    for scan_result in scan_results {
        if let Some(saved_network_info) = saved_networks.get(&scan_result.id) {
            for bss in &scan_result.entries {
                merged_networks.push(InternalBss {
                    bss_info: bss,
                    network_id: scan_result.id.clone(),
                    network_info: saved_network_info.clone(),
                });
            }
        }
    }
    merged_networks
}

/// Insert all saved networks into a hashmap with this module's internal data representation
async fn load_saved_networks(
    saved_network_manager: Arc<SavedNetworksManager>,
) -> HashMap<types::NetworkIdentifier, InternalSavedNetworkData> {
    let mut networks: HashMap<types::NetworkIdentifier, InternalSavedNetworkData> = HashMap::new();
    for saved_network in saved_network_manager.get_networks().await.into_iter() {
        let recent_failure_count = saved_network
            .perf_stats
            .failure_list
            .get_recent(zx::Time::get_monotonic() - RECENT_FAILURE_WINDOW)
            .len()
            .try_into()
            .unwrap_or_else(|e| {
                error!("Failed to convert failure count: {:?}", e);
                u8::MAX
            });

        trace!(
            "Adding saved network to hashmap{}",
            if recent_failure_count > 0 { " with some failures" } else { "" }
        );
        // We allow networks saved as WPA to be also used as WPA2 or WPA2 to be used for WPA3
        if let Some(security_type) = upgrade_security(&saved_network.security_type) {
            networks.insert(
                types::NetworkIdentifier { ssid: saved_network.ssid.clone(), type_: security_type },
                InternalSavedNetworkData {
                    credential: saved_network.credential.clone(),
                    has_ever_connected: saved_network.has_ever_connected,
                    recent_failure_count: recent_failure_count,
                },
            );
        };
        networks.insert(
            types::NetworkIdentifier {
                ssid: saved_network.ssid,
                type_: saved_network.security_type.into(),
            },
            InternalSavedNetworkData {
                credential: saved_network.credential,
                has_ever_connected: saved_network.has_ever_connected,
                recent_failure_count: recent_failure_count,
            },
        );
    }
    networks
}

fn upgrade_security(security: &config_management::SecurityType) -> Option<types::SecurityType> {
    match security {
        config_management::SecurityType::Wpa => Some(types::SecurityType::Wpa2),
        config_management::SecurityType::Wpa2 => Some(types::SecurityType::Wpa3),
        _ => None,
    }
}

pub struct NetworkSelectorScanUpdater {
    scan_result_cache: Arc<Mutex<ScanResultCache>>,
    saved_network_manager: Arc<SavedNetworksManager>,
    cobalt_api: Arc<Mutex<CobaltSender>>,
}
#[async_trait]
impl ScanResultUpdate for NetworkSelectorScanUpdater {
    async fn update_scan_results(&mut self, scan_results: &Vec<types::ScanResult>) {
        // Update internal scan result cache
        let scan_results_clone = scan_results.clone();
        let mut scan_result_guard = self.scan_result_cache.lock().await;
        scan_result_guard.results = scan_results_clone;
        scan_result_guard.updated_at = zx::Time::get_monotonic();
        drop(scan_result_guard);

        // Record metrics for this scan
        let saved_networks = load_saved_networks(Arc::clone(&self.saved_network_manager)).await;
        let mut cobalt_api_guard = self.cobalt_api.lock().await;
        let cobalt_api = &mut *cobalt_api_guard;
        record_metrics_on_scan(scan_results, saved_networks, cobalt_api);
        drop(cobalt_api_guard);
    }
}

fn select_best_connection_candidate<'a>(
    bss_list: Vec<InternalBss<'a>>,
    ignore_list: &Vec<types::NetworkIdentifier>,
) -> Option<(types::ConnectionCandidate, types::WlanChan, types::Bssid)> {
    bss_list
        .into_iter()
        .filter(|bss| {
            // Filter out incompatible BSSs
            if !bss.bss_info.compatible {
                trace!("BSS is incompatible, filtering");
                return false;
            };
            // Filter out networks we've been told to ignore
            if ignore_list.contains(&bss.network_id) {
                trace!("Network is ignored, filtering");
                return false;
            }
            true
        })
        .max_by(|bss_a, bss_b| {
            // If only one network has failures, prefer the other one
            if bss_a.network_info.recent_failure_count > 0
                && bss_b.network_info.recent_failure_count == 0
            {
                return Ordering::Less;
            }
            if bss_a.network_info.recent_failure_count == 0
                && bss_b.network_info.recent_failure_count > 0
            {
                return Ordering::Greater;
            }

            // Both networks have failures, compare their scores
            bss_a.score().partial_cmp(&bss_b.score()).unwrap()
        })
        .map(|bss| {
            (
                types::ConnectionCandidate {
                    network: bss.network_id,
                    credential: bss.network_info.credential,
                    observed_in_passive_scan: Some(bss.bss_info.observed_in_passive_scan),
                    bss: bss.bss_info.bss_desc.clone(),
                },
                bss.bss_info.channel,
                bss.bss_info.bssid,
            )
        })
}

/// If a BSS was discovered via a passive scan, we need to perform an active scan on it to discover
/// all the information potentially needed by the SME layer.
async fn augment_bss_with_active_scan(
    selected_network: types::ConnectionCandidate,
    channel: types::WlanChan,
    bssid: types::Bssid,
    iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
) -> types::ConnectionCandidate {
    // This internal function encapsulates all the logic and has a Result<> return type, allowing us
    // to use the `?` operator inside it to reduce nesting.
    async fn get_enhanced_bss_description(
        selected_network: &types::ConnectionCandidate,
        channel: types::WlanChan,
        bssid: types::Bssid,
        iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
    ) -> Result<Option<Box<fidl_internal::BssDescription>>, ()> {
        // Make sure the scan is needed
        match selected_network.observed_in_passive_scan {
            Some(true) => info!("Performing directed active scan on selected network"),
            Some(false) => {
                debug!("Network already discovered via active scan.");
                return Err(());
            }
            None => {
                error!("Unexpected 'None' value for 'observed_in_passive_scan'.");
                return Err(());
            }
        }

        // Get an SME proxy
        let sme_proxy = iface_manager.lock().await.get_sme_proxy_for_scan().await.map_err(|e| {
            info!("Failed to get an SME proxy for scan: {:?}", e);
        })?;

        // Perform the scan
        let mut directed_scan_result = scan::perform_directed_active_scan(
            &sme_proxy,
            &selected_network.network.ssid,
            Some(vec![channel.primary]),
        )
        .await
        .map_err(|()| {
            info!("Failed to perform active scan to augment BSS info.");
        })?;

        // Find the network in the results
        let mut network = directed_scan_result
            .drain(..)
            .find(|n| n.id == selected_network.network)
            .ok_or_else(|| {
                info!("BSS info will lack active scan augmentation, proceeding anyway.");
            })?;

        // Find the BSS in the network's list of BSSs
        let bss = network.entries.drain(..).find(|bss| bss.bssid == bssid).ok_or_else(|| {
            info!("BSS info will lack active scan augmentation, proceeding anyway.");
        })?;

        Ok(bss.bss_desc)
    }

    match get_enhanced_bss_description(&selected_network, channel, bssid, iface_manager).await {
        Ok(new_bss_desc) => types::ConnectionCandidate { bss: new_bss_desc, ..selected_network },
        Err(()) => selected_network,
    }
}

fn record_metrics_on_scan(
    scan_results: &Vec<types::ScanResult>,
    saved_networks: HashMap<types::NetworkIdentifier, InternalSavedNetworkData>,
    cobalt_api: &mut CobaltSender,
) {
    let mut num_saved_networks_observed = 0;
    let mut num_actively_scanned_networks = 0;

    for scan_result in scan_results {
        if let Some(_) = saved_networks.get(&scan_result.id) {
            // This saved network was present in scan results.
            num_saved_networks_observed += 1;

            // Check if the network was found via active scan.
            if scan_result.entries.iter().any(|bss| bss.observed_in_passive_scan == false) {
                num_actively_scanned_networks += 1;
            };

            // Record how many BSSs are visible in the scan results for this saved network.
            let num_bss = match scan_result.entries.len() {
                0 => unreachable!(), // The ::Zero enum exists, but we shouldn't get a scan result with no BSS
                1 => SavedNetworkInScanResultMetricDimensionBssCount::One,
                2..=4 => SavedNetworkInScanResultMetricDimensionBssCount::TwoToFour,
                5..=10 => SavedNetworkInScanResultMetricDimensionBssCount::FiveToTen,
                11..=20 => SavedNetworkInScanResultMetricDimensionBssCount::ElevenToTwenty,
                21..=usize::MAX => SavedNetworkInScanResultMetricDimensionBssCount::TwentyOneOrMore,
                _ => unreachable!(),
            };
            cobalt_api.log_event(SAVED_NETWORK_IN_SCAN_RESULT_METRIC_ID, num_bss);
        }
    }

    let saved_network_count_metric = match num_saved_networks_observed {
        0 => ScanResultsReceivedMetricDimensionSavedNetworksCount::Zero,
        1 => ScanResultsReceivedMetricDimensionSavedNetworksCount::One,
        2..=4 => ScanResultsReceivedMetricDimensionSavedNetworksCount::TwoToFour,
        5..=20 => ScanResultsReceivedMetricDimensionSavedNetworksCount::FiveToTwenty,
        21..=40 => ScanResultsReceivedMetricDimensionSavedNetworksCount::TwentyOneToForty,
        41..=usize::MAX => ScanResultsReceivedMetricDimensionSavedNetworksCount::FortyOneOrMore,
        _ => unreachable!(),
    };
    cobalt_api.log_event(SCAN_RESULTS_RECEIVED_METRIC_ID, saved_network_count_metric);

    let actively_scanned_networks_metrics = match num_actively_scanned_networks {
        0 => ActiveScanSsidsObserved::Zero,
        1 => ActiveScanSsidsObserved::One,
        2..=4 => ActiveScanSsidsObserved::TwoToFour,
        5..=10 => ActiveScanSsidsObserved::FiveToTen,
        11..=20 => ActiveScanSsidsObserved::ElevenToTwenty,
        21..=50 => ActiveScanSsidsObserved::TwentyOneToFifty,
        51..=100 => ActiveScanSsidsObserved::FiftyOneToOneHundred,
        101..=usize::MAX => ActiveScanSsidsObserved::OneHundredAndOneOrMore,
        _ => unreachable!(),
    };
    cobalt_api.log_event(
        SAVED_NETWORK_IN_SCAN_RESULT_WITH_ACTIVE_SCAN_METRIC_ID,
        actively_scanned_networks_metrics,
    );
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            access_point::state_machine as ap_fsm,
            util::{
                logger::set_logger_for_test,
                testing::{
                    create_mock_cobalt_sender_and_receiver, generate_channel,
                    generate_random_bss_desc, generate_random_channel,
                    validate_sme_scan_request_and_send_results,
                },
            },
        },
        anyhow::Error,
        cobalt_client::traits::AsEventCode,
        fidl::endpoints::create_proxy,
        fidl_fuchsia_cobalt::CobaltEvent,
        fidl_fuchsia_wlan_common as fidl_common, fidl_fuchsia_wlan_sme as fidl_sme,
        fuchsia_async::{self as fasync, DurationExt},
        fuchsia_cobalt::cobalt_event_builder::CobaltEventExt,
        futures::{
            channel::{mpsc, oneshot},
            prelude::*,
            task::Poll,
        },
        pin_utils::pin_mut,
        rand::Rng,
        std::sync::Arc,
        test_case::test_case,
        wlan_common::assert_variant,
    };

    struct TestValues {
        network_selector: Arc<NetworkSelector>,
        saved_network_manager: Arc<SavedNetworksManager>,
        cobalt_events: mpsc::Receiver<CobaltEvent>,
        iface_manager: Arc<Mutex<FakeIfaceManager>>,
        sme_stream: fidl_sme::ClientSmeRequestStream,
    }

    async fn test_setup() -> TestValues {
        set_logger_for_test();

        // setup modules
        let (cobalt_api, cobalt_events) = create_mock_cobalt_sender_and_receiver();
        let saved_network_manager = Arc::new(SavedNetworksManager::new_for_test().await.unwrap());
        let network_selector =
            Arc::new(NetworkSelector::new(Arc::clone(&saved_network_manager), cobalt_api));
        let (client_sme, remote) =
            create_proxy::<fidl_sme::ClientSmeMarker>().expect("error creating proxy");
        let iface_manager = Arc::new(Mutex::new(FakeIfaceManager::new(client_sme)));

        TestValues {
            network_selector,
            saved_network_manager,
            cobalt_events,
            iface_manager,
            sme_stream: remote.into_stream().expect("failed to create stream"),
        }
    }

    struct FakeIfaceManager {
        pub sme_proxy: fidl_fuchsia_wlan_sme::ClientSmeProxy,
    }

    impl FakeIfaceManager {
        pub fn new(proxy: fidl_fuchsia_wlan_sme::ClientSmeProxy) -> Self {
            FakeIfaceManager { sme_proxy: proxy }
        }
    }

    #[async_trait]
    impl IfaceManagerApi for FakeIfaceManager {
        async fn disconnect(
            &mut self,
            _network_id: fidl_fuchsia_wlan_policy::NetworkIdentifier,
            _reason: types::DisconnectReason,
        ) -> Result<(), Error> {
            unimplemented!()
        }

        async fn connect(
            &mut self,
            _connect_req: types::ConnectRequest,
        ) -> Result<oneshot::Receiver<()>, Error> {
            unimplemented!()
        }

        async fn record_idle_client(&mut self, _iface_id: u16) -> Result<(), Error> {
            unimplemented!()
        }

        async fn has_idle_client(&mut self) -> Result<bool, Error> {
            unimplemented!()
        }

        async fn handle_added_iface(&mut self, _iface_id: u16) -> Result<(), Error> {
            unimplemented!()
        }

        async fn handle_removed_iface(&mut self, _iface_id: u16) -> Result<(), Error> {
            unimplemented!()
        }

        async fn scan(
            &mut self,
            mut scan_request: fidl_sme::ScanRequest,
        ) -> Result<fidl_fuchsia_wlan_sme::ScanTransactionProxy, Error> {
            let (local, remote) = fidl::endpoints::create_proxy()?;
            let _ = self.sme_proxy.scan(&mut scan_request, remote);
            Ok(local)
        }

        async fn get_sme_proxy_for_scan(
            &mut self,
        ) -> Result<fidl_fuchsia_wlan_sme::ClientSmeProxy, Error> {
            Ok(self.sme_proxy.clone())
        }

        async fn stop_client_connections(
            &mut self,
            _reason: types::DisconnectReason,
        ) -> Result<(), Error> {
            unimplemented!()
        }

        async fn start_client_connections(&mut self) -> Result<(), Error> {
            unimplemented!()
        }

        async fn start_ap(
            &mut self,
            _config: ap_fsm::ApConfig,
        ) -> Result<oneshot::Receiver<()>, Error> {
            unimplemented!()
        }

        async fn stop_ap(&mut self, _ssid: Vec<u8>, _password: Vec<u8>) -> Result<(), Error> {
            unimplemented!()
        }

        async fn stop_all_aps(&mut self) -> Result<(), Error> {
            unimplemented!()
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn saved_networks_are_loaded() {
        let test_values = test_setup().await;

        // check there are 0 saved networks to start with
        let networks = load_saved_networks(Arc::clone(&test_values.saved_network_manager)).await;
        assert_eq!(networks.len(), 0);

        // create some identifiers
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let ssid_2 = "bar".as_bytes().to_vec();
        let test_id_2 =
            types::NetworkIdentifier { ssid: ssid_2.clone(), type_: types::SecurityType::Wpa };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());

        // insert some new saved networks
        test_values
            .saved_network_manager
            .store(test_id_1.clone().into(), credential_1.clone())
            .await
            .unwrap();

        test_values
            .saved_network_manager
            .store(test_id_2.clone().into(), credential_2.clone())
            .await
            .unwrap();

        // mark the first one as having connected
        test_values
            .saved_network_manager
            .record_connect_result(
                test_id_1.clone().into(),
                &credential_1.clone(),
                fidl_sme::ConnectResultCode::Success,
                None,
            )
            .await;

        // mark the second one as having a failure
        test_values
            .saved_network_manager
            .record_connect_result(
                test_id_2.clone().into(),
                &credential_2.clone(),
                fidl_sme::ConnectResultCode::CredentialRejected,
                None,
            )
            .await;

        // check these networks were loaded
        let mut expected_hashmap = HashMap::new();
        expected_hashmap.insert(
            test_id_1,
            InternalSavedNetworkData {
                credential: credential_1,
                has_ever_connected: true,
                recent_failure_count: 0,
            },
        );
        expected_hashmap.insert(
            test_id_2,
            InternalSavedNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: false,
                recent_failure_count: 1,
            },
        );
        // Networks saved as WPA can be used to auto connect to WPA2 networks
        expected_hashmap.insert(
            types::NetworkIdentifier { ssid: ssid_2, type_: types::SecurityType::Wpa2 },
            InternalSavedNetworkData {
                credential: credential_2,
                has_ever_connected: false,
                recent_failure_count: 1,
            },
        );
        let networks = load_saved_networks(Arc::clone(&test_values.saved_network_manager)).await;
        assert_eq!(networks, expected_hashmap);
    }

    #[fasync::run_singlethreaded(test)]
    async fn scan_results_are_stored() {
        let mut test_values = test_setup().await;
        let network_selector = test_values.network_selector;

        // check there are 0 scan results to start with
        let guard = network_selector.scan_result_cache.lock().await;
        assert_eq!(guard.results.len(), 0);
        drop(guard);

        // create some identifiers
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };

        // provide some new scan results
        let mock_scan_results = vec![
            types::ScanResult {
                id: test_id_1.clone(),
                entries: vec![generate_random_bss(), generate_random_bss(), generate_random_bss()],
                compatibility: types::Compatibility::Supported,
            },
            types::ScanResult {
                id: test_id_2.clone(),
                entries: vec![generate_random_bss()],
                compatibility: types::Compatibility::DisallowedNotSupported,
            },
        ];
        let mut updater = network_selector.generate_scan_result_updater();
        updater.update_scan_results(&mock_scan_results).await;

        // check that the scan results are stored
        let guard = network_selector.scan_result_cache.lock().await;
        assert_eq!(guard.results, mock_scan_results);

        // check there are some metric events for the incoming scan results
        // note: the actual metrics are checked in unit tests for the metric recording function
        assert!(test_values.cobalt_events.try_next().unwrap().is_some());
    }

    #[fasync::run_singlethreaded(test)]
    async fn scan_results_merged_with_saved_networks() {
        // create some identifiers
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());

        // create the saved networks hashmap
        let mut saved_networks = HashMap::new();
        saved_networks.insert(
            test_id_1.clone(),
            InternalSavedNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                recent_failure_count: 0,
            },
        );
        saved_networks.insert(
            test_id_2.clone(),
            InternalSavedNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: false,
                recent_failure_count: 0,
            },
        );

        // build some scan results
        let mock_scan_results = vec![
            types::ScanResult {
                id: test_id_1.clone(),
                entries: vec![generate_random_bss(), generate_random_bss(), generate_random_bss()],
                compatibility: types::Compatibility::Supported,
            },
            types::ScanResult {
                id: test_id_2.clone(),
                entries: vec![generate_random_bss()],
                compatibility: types::Compatibility::DisallowedNotSupported,
            },
        ];

        // build our expected result
        let expected_result = vec![
            InternalBss {
                network_id: test_id_1.clone(),
                network_info: InternalSavedNetworkData {
                    credential: credential_1.clone(),
                    has_ever_connected: true,
                    recent_failure_count: 0,
                },
                bss_info: &mock_scan_results[0].entries[0],
            },
            InternalBss {
                network_id: test_id_1.clone(),
                network_info: InternalSavedNetworkData {
                    credential: credential_1.clone(),
                    has_ever_connected: true,
                    recent_failure_count: 0,
                },
                bss_info: &mock_scan_results[0].entries[1],
            },
            InternalBss {
                network_id: test_id_1.clone(),
                network_info: InternalSavedNetworkData {
                    credential: credential_1.clone(),
                    has_ever_connected: true,
                    recent_failure_count: 0,
                },
                bss_info: &mock_scan_results[0].entries[2],
            },
            InternalBss {
                network_id: test_id_2.clone(),
                network_info: InternalSavedNetworkData {
                    credential: credential_2.clone(),
                    has_ever_connected: false,
                    recent_failure_count: 0,
                },
                bss_info: &mock_scan_results[1].entries[0],
            },
        ];

        // validate the function works
        let result = merge_saved_networks_and_scan_data(saved_networks, &mock_scan_results).await;
        assert_eq!(result, expected_result);
    }

    #[test_case(types::Bss {
            rssi: -8,
            channel: generate_channel(1),
            ..generate_random_bss()
        },
        -8; "2.4GHz BSS score is RSSI")]
    #[test_case(types::Bss {
            rssi: -49,
            channel: generate_channel(36),
            ..generate_random_bss()
        },
        -34; "5GHz score is (RSSI + mod), when above threshold")]
    #[test_case(types::Bss {
            rssi: -51,
            channel: generate_channel(36),
            ..generate_random_bss()
        },
        -51; "5GHz score is RSSI, when below threshold")]
    fn scoring_test(bss: types::Bss, expected_score: i8) {
        let mut rng = rand::thread_rng();

        let internal_bss = InternalBss {
            network_id: types::NetworkIdentifier {
                ssid: "test".as_bytes().to_vec(),
                type_: types::SecurityType::Wpa3,
            },
            network_info: InternalSavedNetworkData {
                credential: Credential::None,
                has_ever_connected: rng.gen::<bool>(),
                recent_failure_count: rng.gen_range(0, 20),
            },
            bss_info: &bss,
        };

        assert_eq!(internal_bss.score(), expected_score)
    }

    #[test]
    fn select_best_connection_candidate_sorts_by_score() {
        // build networks list
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());

        let mut networks = vec![];

        let bss_info1 = types::Bss {
            compatible: true,
            rssi: -14,
            channel: generate_channel(36),
            ..generate_random_bss()
        };
        networks.push(InternalBss {
            network_id: test_id_1.clone(),
            network_info: InternalSavedNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                recent_failure_count: 0,
            },
            bss_info: &bss_info1,
        });

        let bss_info2 = types::Bss {
            compatible: true,
            rssi: -10,
            channel: generate_channel(1),
            ..generate_random_bss()
        };
        networks.push(InternalBss {
            network_id: test_id_1.clone(),
            network_info: InternalSavedNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                recent_failure_count: 0,
            },
            bss_info: &bss_info2,
        });

        let bss_info3 = types::Bss {
            compatible: true,
            rssi: -8,
            channel: generate_channel(1),
            ..generate_random_bss()
        };
        networks.push(InternalBss {
            network_id: test_id_2.clone(),
            network_info: InternalSavedNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: true,
                recent_failure_count: 0,
            },
            bss_info: &bss_info3,
        });

        // there's a network on 5G, it should get a boost and be selected
        assert_eq!(
            select_best_connection_candidate(networks.clone(), &vec![]),
            Some((
                types::ConnectionCandidate {
                    network: test_id_1.clone(),
                    credential: credential_1.clone(),
                    bss: bss_info1.bss_desc.clone(),
                    observed_in_passive_scan: Some(bss_info1.observed_in_passive_scan),
                },
                bss_info1.channel,
                bss_info1.bssid
            ))
        );

        // make the 5GHz network into a 2.4GHz network
        let mut modified_network = networks[0].clone();
        let modified_bss_info =
            types::Bss { channel: generate_channel(6), ..modified_network.bss_info.clone() };
        modified_network.bss_info = &modified_bss_info;
        networks[0] = modified_network;

        // all networks are 2.4GHz, strongest RSSI network returned
        assert_eq!(
            select_best_connection_candidate(networks.clone(), &vec![]),
            Some((
                types::ConnectionCandidate {
                    network: test_id_2.clone(),
                    credential: credential_2.clone(),
                    bss: networks[2].bss_info.bss_desc.clone(),
                    observed_in_passive_scan: Some(networks[2].bss_info.observed_in_passive_scan),
                },
                networks[2].bss_info.channel,
                networks[2].bss_info.bssid
            ))
        );
    }

    #[test]
    fn select_best_connection_candidate_sorts_by_failure_count() {
        // build networks list
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());

        let mut networks = vec![];

        let bss_info1 = types::Bss { compatible: true, rssi: -14, ..generate_random_bss() };
        networks.push(InternalBss {
            network_id: test_id_1.clone(),
            network_info: InternalSavedNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                recent_failure_count: 0,
            },
            bss_info: &bss_info1,
        });

        let bss_info2 = types::Bss { compatible: true, rssi: -100, ..generate_random_bss() };
        networks.push(InternalBss {
            network_id: test_id_2.clone(),
            network_info: InternalSavedNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: true,
                recent_failure_count: 0,
            },
            bss_info: &bss_info2,
        });

        // stronger network returned
        assert_eq!(
            select_best_connection_candidate(networks.clone(), &vec![]),
            Some((
                types::ConnectionCandidate {
                    network: test_id_1.clone(),
                    credential: credential_1.clone(),
                    bss: bss_info1.bss_desc.clone(),
                    observed_in_passive_scan: Some(networks[0].bss_info.observed_in_passive_scan)
                },
                bss_info1.channel,
                bss_info1.bssid
            ))
        );

        // mark the stronger network as having a failure
        let mut modified_network = networks[0].clone();
        modified_network.network_info.recent_failure_count = 2;
        networks[0] = modified_network;

        // weaker network (with no failures) returned
        assert_eq!(
            select_best_connection_candidate(networks.clone(), &vec![]),
            Some((
                types::ConnectionCandidate {
                    network: test_id_2.clone(),
                    credential: credential_2.clone(),
                    bss: bss_info2.bss_desc.clone(),
                    observed_in_passive_scan: Some(networks[1].bss_info.observed_in_passive_scan)
                },
                bss_info2.channel,
                bss_info2.bssid
            ))
        );

        // give them both the same number of failures
        let mut modified_network = networks[1].clone();
        modified_network.network_info.recent_failure_count = 2;
        networks[1] = modified_network;

        // stronger network returned
        assert_eq!(
            select_best_connection_candidate(networks.clone(), &vec![]),
            Some((
                types::ConnectionCandidate {
                    network: test_id_1.clone(),
                    credential: credential_1.clone(),
                    bss: bss_info1.bss_desc.clone(),
                    observed_in_passive_scan: Some(networks[0].bss_info.observed_in_passive_scan)
                },
                bss_info1.channel,
                bss_info1.bssid
            ))
        );
    }

    #[test]
    fn select_best_connection_candidate_incompatible() {
        // build networks list
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());

        let mut networks = vec![];

        let bss_info1 = types::Bss {
            compatible: true,
            rssi: -14,
            channel: generate_channel(1),
            ..generate_random_bss()
        };
        networks.push(InternalBss {
            network_id: test_id_1.clone(),
            network_info: InternalSavedNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                recent_failure_count: 0,
            },
            bss_info: &bss_info1,
        });

        let bss_info2 = types::Bss {
            compatible: false,
            rssi: -10,
            channel: generate_channel(1),
            ..generate_random_bss()
        };
        networks.push(InternalBss {
            network_id: test_id_1.clone(),
            network_info: InternalSavedNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                recent_failure_count: 0,
            },
            bss_info: &bss_info2,
        });

        let bss_info3 = types::Bss {
            compatible: true,
            rssi: -12,
            channel: generate_channel(1),
            ..generate_random_bss()
        };
        networks.push(InternalBss {
            network_id: test_id_2.clone(),
            network_info: InternalSavedNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: true,
                recent_failure_count: 0,
            },
            bss_info: &bss_info3,
        });

        // stronger network returned
        assert_eq!(
            select_best_connection_candidate(networks.clone(), &vec![]),
            Some((
                types::ConnectionCandidate {
                    network: test_id_2.clone(),
                    credential: credential_2.clone(),
                    bss: bss_info3.bss_desc.clone(),
                    observed_in_passive_scan: Some(networks[2].bss_info.observed_in_passive_scan),
                },
                bss_info3.channel,
                bss_info3.bssid
            ))
        );

        // mark the stronger network as incompatible
        let mut modified_network = networks[2].clone();
        let modified_bss_info =
            types::Bss { compatible: false, ..modified_network.bss_info.clone() };
        modified_network.bss_info = &modified_bss_info;
        networks[2] = modified_network;

        // other network returned
        assert_eq!(
            select_best_connection_candidate(networks.clone(), &vec![]),
            Some((
                types::ConnectionCandidate {
                    network: test_id_1.clone(),
                    credential: credential_1.clone(),
                    bss: networks[0].bss_info.bss_desc.clone(),
                    observed_in_passive_scan: Some(networks[0].bss_info.observed_in_passive_scan),
                },
                networks[0].bss_info.channel,
                networks[0].bss_info.bssid
            ))
        );
    }

    #[test]
    fn select_best_connection_candidate_ignore_list() {
        // build networks list
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());

        let mut networks = vec![];

        let bss_info1 = types::Bss { compatible: true, rssi: -100, ..generate_random_bss() };
        networks.push(InternalBss {
            network_id: test_id_1.clone(),
            network_info: InternalSavedNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                recent_failure_count: 0,
            },
            bss_info: &bss_info1,
        });

        let bss_info2 = types::Bss { compatible: true, rssi: -12, ..generate_random_bss() };
        networks.push(InternalBss {
            network_id: test_id_2.clone(),
            network_info: InternalSavedNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: true,
                recent_failure_count: 0,
            },
            bss_info: &bss_info2,
        });

        // stronger network returned
        assert_eq!(
            select_best_connection_candidate(networks.clone(), &vec![]),
            Some((
                types::ConnectionCandidate {
                    network: test_id_2.clone(),
                    credential: credential_2.clone(),
                    bss: bss_info2.bss_desc.clone(),
                    observed_in_passive_scan: Some(networks[1].bss_info.observed_in_passive_scan),
                },
                bss_info2.channel,
                bss_info2.bssid
            ))
        );

        // ignore the stronger network, other network returned
        assert_eq!(
            select_best_connection_candidate(networks.clone(), &vec![test_id_2.clone()]),
            Some((
                types::ConnectionCandidate {
                    network: test_id_1.clone(),
                    credential: credential_1.clone(),
                    bss: bss_info1.bss_desc.clone(),
                    observed_in_passive_scan: Some(networks[0].bss_info.observed_in_passive_scan),
                },
                bss_info1.channel,
                bss_info1.bssid
            ))
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn perform_scan_cache_is_fresh() {
        let mut test_values = test_setup().await;
        let network_selector = test_values.network_selector;

        // Set the scan result cache to be fresher than STALE_SCAN_AGE
        let mut scan_result_guard = network_selector.scan_result_cache.lock().await;
        let last_scan_age = zx::Duration::from_seconds(1);
        assert!(last_scan_age < STALE_SCAN_AGE);
        scan_result_guard.updated_at = zx::Time::get_monotonic() - last_scan_age;
        drop(scan_result_guard);

        network_selector.perform_scan(test_values.iface_manager).await;

        // Metric logged for scan age
        let metric = test_values.cobalt_events.try_next().unwrap().unwrap();
        let expected_metric =
            CobaltEvent::builder(LAST_SCAN_AGE_WHEN_SCAN_REQUESTED_METRIC_ID).as_elapsed_time(0);
        // We need to individually check each field, since the elapsed time is non-deterministic
        assert_eq!(metric.metric_id, expected_metric.metric_id);
        assert_eq!(metric.event_codes, expected_metric.event_codes);
        assert_eq!(metric.component, expected_metric.component);
        assert_variant!(
            metric.payload, fidl_fuchsia_cobalt::EventPayload::ElapsedMicros(elapsed_micros) => {
                let elapsed_time = zx::Duration::from_micros(elapsed_micros.try_into().unwrap());
                assert!(elapsed_time < STALE_SCAN_AGE);
            }
        );

        // No scan performed
        assert!(test_values.sme_stream.next().await.is_none());
    }

    #[test]
    fn perform_scan_cache_is_stale() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = exec.run_singlethreaded(test_setup());
        let network_selector = test_values.network_selector;
        let test_start_time = zx::Time::get_monotonic();

        // Set the scan result cache to be older than STALE_SCAN_AGE
        let mut scan_result_guard =
            exec.run_singlethreaded(network_selector.scan_result_cache.lock());
        scan_result_guard.updated_at =
            zx::Time::get_monotonic() - (STALE_SCAN_AGE + zx::Duration::from_seconds(1));
        drop(scan_result_guard);

        // Kick off scan
        let scan_fut = network_selector.perform_scan(test_values.iface_manager);
        pin_mut!(scan_fut);
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Metric logged for scan age
        let metric = test_values.cobalt_events.try_next().unwrap().unwrap();
        let expected_metric =
            CobaltEvent::builder(LAST_SCAN_AGE_WHEN_SCAN_REQUESTED_METRIC_ID).as_elapsed_time(0);
        assert_eq!(metric.metric_id, expected_metric.metric_id);
        assert_eq!(metric.event_codes, expected_metric.event_codes);
        assert_eq!(metric.component, expected_metric.component);
        assert_variant!(
            metric.payload, fidl_fuchsia_cobalt::EventPayload::ElapsedMicros(elapsed_micros) => {
                let elapsed_time = zx::Duration::from_micros(elapsed_micros.try_into().unwrap());
                assert!(elapsed_time > STALE_SCAN_AGE);
            }
        );

        // Check that a scan request was sent to the sme and send back results
        let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut test_values.sme_stream,
            &expected_scan_request,
            vec![],
        );
        // Process scan
        exec.run_singlethreaded(&mut scan_fut);

        // Check scan results were updated
        let scan_result_guard = exec.run_singlethreaded(network_selector.scan_result_cache.lock());
        assert!(scan_result_guard.updated_at > test_start_time);
        assert!(scan_result_guard.updated_at < zx::Time::get_monotonic());
        drop(scan_result_guard);
    }

    #[test]
    fn augment_bss_with_active_scan_doesnt_run_on_actively_found_networks() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let test_values = exec.run_singlethreaded(test_setup());

        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let bss_info1 = types::Bss {
            compatible: true,
            rssi: -14,
            channel: generate_channel(36),
            ..generate_random_bss()
        };
        let connect_req = types::ConnectionCandidate {
            network: test_id_1.clone(),
            credential: credential_1.clone(),
            bss: bss_info1.bss_desc.clone(),
            observed_in_passive_scan: Some(false), // was actively scanned
        };

        let fut = augment_bss_with_active_scan(
            connect_req.clone(),
            bss_info1.channel,
            bss_info1.bssid,
            test_values.iface_manager.clone(),
        );
        pin_mut!(fut);

        // The connect_req comes out the other side with no change
        assert_variant!(exec.run_until_stalled(&mut fut), Poll::Ready(req) => {
            assert_eq!(req, connect_req)}
        );
    }

    #[test]
    fn augment_bss_with_active_scan_runs_on_passively_found_networks() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = exec.run_singlethreaded(test_setup());

        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let bss_info1 = types::Bss {
            compatible: true,
            rssi: -14,
            channel: generate_channel(36),
            ..generate_random_bss()
        };
        let connect_req = types::ConnectionCandidate {
            network: test_id_1.clone(),
            credential: credential_1.clone(),
            bss: bss_info1.bss_desc.clone(),
            observed_in_passive_scan: Some(true), // was passively scanned
        };

        let fut = augment_bss_with_active_scan(
            connect_req.clone(),
            bss_info1.channel,
            bss_info1.bssid,
            test_values.iface_manager.clone(),
        );
        pin_mut!(fut);

        // Progress the future until a scan request is sent
        assert_variant!(exec.run_until_stalled(&mut fut), Poll::Pending);

        // Check that a scan request was sent to the sme and send back results
        let expected_scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
            ssids: vec![test_id_1.ssid.clone()],
            channels: vec![36],
        });
        let new_bss_desc = generate_random_bss_desc();
        let mock_scan_results = vec![
            fidl_sme::BssInfo {
                bssid: [0, 0, 0, 0, 0, 0], // Not the same BSSID
                ssid: test_id_1.ssid.clone(),
                rssi_dbm: 10,
                snr_db: 10,
                channel: fidl_common::WlanChan {
                    primary: 1,
                    cbw: fidl_common::Cbw::Cbw20,
                    secondary80: 0,
                },
                protection: fidl_sme::Protection::Wpa3Enterprise,
                compatible: true,
                bss_desc: generate_random_bss_desc(),
            },
            fidl_sme::BssInfo {
                bssid: bss_info1.bssid.clone(),
                ssid: test_id_1.ssid.clone(),
                rssi_dbm: 0,
                snr_db: 0,
                channel: fidl_common::WlanChan {
                    primary: 1,
                    cbw: fidl_common::Cbw::Cbw20,
                    secondary80: 0,
                },
                protection: fidl_sme::Protection::Wpa3Enterprise,
                compatible: true,
                bss_desc: new_bss_desc.clone(),
            },
        ];
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut test_values.sme_stream,
            &expected_scan_request,
            mock_scan_results,
        );

        // The connect_req comes out the other side with the new bss_desc
        assert_eq!(
            exec.run_singlethreaded(fut),
            types::ConnectionCandidate {
                bss: new_bss_desc,
                // observed_in_passive_scan should still be true, since the network was found in a
                // passive scan prior to the directed active scan augmentation.
                observed_in_passive_scan: Some(true),
                ..connect_req
            }
        );
    }

    #[test]
    fn find_best_connection_candidate_end_to_end() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = exec.run_singlethreaded(test_setup());
        let network_selector = test_values.network_selector;

        // create some identifiers
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let bss_desc1 = generate_random_bss_desc();
        let bss_desc1_active = generate_random_bss_desc();
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());
        let bss_desc2 = generate_random_bss_desc();
        let bss_desc2_active = generate_random_bss_desc();

        // insert some new saved networks
        exec.run_singlethreaded(
            test_values.saved_network_manager.store(test_id_1.clone().into(), credential_1.clone()),
        )
        .unwrap();
        exec.run_singlethreaded(
            test_values.saved_network_manager.store(test_id_2.clone().into(), credential_2.clone()),
        )
        .unwrap();

        // Mark them as having connected. Make connection passive so that no active scans are made.
        exec.run_singlethreaded(test_values.saved_network_manager.record_connect_result(
            test_id_1.clone().into(),
            &credential_1.clone(),
            fidl_sme::ConnectResultCode::Success,
            Some(fidl_common::ScanType::Passive),
        ));
        exec.run_singlethreaded(test_values.saved_network_manager.record_connect_result(
            test_id_2.clone().into(),
            &credential_2.clone(),
            fidl_sme::ConnectResultCode::Success,
            Some(fidl_common::ScanType::Passive),
        ));

        // Kick off network selection
        let ignore_list = vec![];
        let network_selection_fut = network_selector
            .find_best_connection_candidate(test_values.iface_manager.clone(), &ignore_list);
        pin_mut!(network_selection_fut);
        assert_variant!(exec.run_until_stalled(&mut network_selection_fut), Poll::Pending);

        // Check that a scan request was sent to the sme and send back results
        let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        let mock_scan_results = vec![
            fidl_sme::BssInfo {
                bssid: [0, 0, 0, 0, 0, 0],
                ssid: test_id_1.ssid.clone(),
                rssi_dbm: 10,
                snr_db: 10,
                channel: fidl_common::WlanChan {
                    primary: 1,
                    cbw: fidl_common::Cbw::Cbw20,
                    secondary80: 0,
                },
                protection: fidl_sme::Protection::Wpa3Enterprise,
                compatible: true,
                bss_desc: bss_desc1.clone(),
            },
            fidl_sme::BssInfo {
                bssid: [0, 0, 0, 0, 0, 0],
                ssid: test_id_2.ssid.clone(),
                rssi_dbm: 0,
                snr_db: 0,
                channel: fidl_common::WlanChan {
                    primary: 1,
                    cbw: fidl_common::Cbw::Cbw20,
                    secondary80: 0,
                },
                protection: fidl_sme::Protection::Wpa1,
                compatible: true,
                bss_desc: bss_desc2.clone(),
            },
        ];
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut test_values.sme_stream,
            &expected_scan_request,
            mock_scan_results,
        );

        // Process scan results
        assert_variant!(exec.run_until_stalled(&mut network_selection_fut), Poll::Pending);
        // Sleep very briefly to allow the scan module to finish. The fact that the location
        // sensor is non existent and its channel for receiving scan results doesn't work seems
        // to take a brief moment to propagate, and allow the scan process to finish.
        // TODO(fxbug.dev/65923): This is a hack but I'm not sure how else to handle it.
        let sleep_duration = zx::Duration::from_millis(10);
        exec.run_singlethreaded(fasync::Timer::new(sleep_duration.after_now()));
        assert_variant!(exec.run_until_stalled(&mut network_selection_fut), Poll::Pending);

        // An additional directed active scan should be made for the selected network
        let expected_scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
            ssids: vec![test_id_1.ssid.clone()],
            channels: vec![1],
        });
        let mock_scan_results = vec![fidl_sme::BssInfo {
            bssid: [0, 0, 0, 0, 0, 0],
            ssid: test_id_1.ssid.clone(),
            rssi_dbm: 10,
            snr_db: 10,
            channel: fidl_common::WlanChan {
                primary: 1,
                cbw: fidl_common::Cbw::Cbw20,
                secondary80: 0,
            },
            protection: fidl_sme::Protection::Wpa3Enterprise,
            compatible: true,
            bss_desc: bss_desc1_active.clone(),
        }];
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut test_values.sme_stream,
            &expected_scan_request,
            mock_scan_results,
        );

        // Check that we pick a network
        let results = exec.run_singlethreaded(&mut network_selection_fut);
        assert_eq!(
            results,
            Some(types::ConnectionCandidate {
                network: test_id_1.clone(),
                credential: credential_1.clone(),
                bss: bss_desc1_active.clone(),
                observed_in_passive_scan: Some(true)
            })
        );

        // Ignore that network, check that we pick the other one
        let ignore_list = vec![test_id_1.clone()];
        let network_selection_fut = network_selector
            .find_best_connection_candidate(test_values.iface_manager.clone(), &ignore_list);
        pin_mut!(network_selection_fut);
        assert_variant!(exec.run_until_stalled(&mut network_selection_fut), Poll::Pending);

        // An additional directed active scan should be made for the selected network
        let expected_scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
            ssids: vec![test_id_2.ssid.clone()],
            channels: vec![1],
        });
        let mock_scan_results = vec![fidl_sme::BssInfo {
            bssid: [0, 0, 0, 0, 0, 0],
            ssid: test_id_2.ssid.clone(),
            rssi_dbm: 10,
            snr_db: 10,
            channel: fidl_common::WlanChan {
                primary: 1,
                cbw: fidl_common::Cbw::Cbw20,
                secondary80: 0,
            },
            protection: fidl_sme::Protection::Wpa1,
            compatible: true,
            bss_desc: bss_desc2_active.clone(),
        }];
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut test_values.sme_stream,
            &expected_scan_request,
            mock_scan_results,
        );

        let results = exec.run_singlethreaded(&mut network_selection_fut);
        assert_eq!(
            results,
            Some(types::ConnectionCandidate {
                network: test_id_2.clone(),
                credential: credential_2.clone(),
                bss: bss_desc2_active.clone(),
                observed_in_passive_scan: Some(true)
            })
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn find_best_connection_candidate_wpa_wpa2() {
        // Check that if we see a WPA2 network and have WPA and WPA3 credentials saved for it, we
        // could choose the WPA credential but not the WPA3 credential. In other words we can
        // upgrade saved networks to higher security but not downgrade.
        let test_values = test_setup().await;
        let network_selector = test_values.network_selector;

        // Save networks with WPA and WPA3 security, same SSIDs, and different passwords.
        let ssid = "foo".as_bytes().to_vec();
        let wpa_network_id =
            types::NetworkIdentifier { ssid: ssid.clone(), type_: types::SecurityType::Wpa };
        let credential = Credential::Password("foo_password".as_bytes().to_vec());
        test_values
            .saved_network_manager
            .store(wpa_network_id.clone().into(), credential.clone())
            .await
            .expect("Failed to save network");
        let wpa3_network_id =
            types::NetworkIdentifier { ssid: ssid.clone(), type_: types::SecurityType::Wpa3 };
        let wpa3_credential = Credential::Password("wpa3_only_password".as_bytes().to_vec());
        test_values
            .saved_network_manager
            .store(wpa3_network_id.clone().into(), wpa3_credential.clone())
            .await
            .expect("Failed to save network");

        // Record passive connects so that the test will not active scan.
        test_values
            .saved_network_manager
            .record_connect_result(
                wpa_network_id.clone().into(),
                &credential,
                fidl_sme::ConnectResultCode::Success,
                Some(fidl_common::ScanType::Passive),
            )
            .await;
        test_values
            .saved_network_manager
            .record_connect_result(
                wpa3_network_id.clone().into(),
                &wpa3_credential,
                fidl_sme::ConnectResultCode::Success,
                Some(fidl_common::ScanType::Passive),
            )
            .await;

        // Feed scans with WPA2 and WPA3 results to network selector, as we should get if a
        // WPA2/WPA3 network was seen.
        let id = types::NetworkIdentifier { ssid: ssid, type_: types::SecurityType::Wpa2 };
        let mixed_scan_results = vec![types::ScanResult {
            id: id.clone(),
            entries: vec![types::Bss {
                compatible: true,
                observed_in_passive_scan: false, // mark this as active, to avoid an additional scan
                ..generate_random_bss()
            }],
            compatibility: types::Compatibility::Supported,
        }];
        let mut updater = network_selector.generate_scan_result_updater();
        updater.update_scan_results(&mixed_scan_results).await;

        // Check that we choose the config saved as WPA2
        assert_eq!(
            network_selector
                .find_best_connection_candidate(test_values.iface_manager.clone(), &vec![])
                .await,
            Some(types::ConnectionCandidate {
                network: id.clone(),
                credential,
                bss: mixed_scan_results[0].entries[0].bss_desc.clone(),
                observed_in_passive_scan: Some(
                    mixed_scan_results[0].entries[0].observed_in_passive_scan
                ),
            })
        );
        assert_eq!(
            network_selector
                .find_best_connection_candidate(test_values.iface_manager, &vec![id])
                .await,
            None
        );
    }

    #[test]
    fn find_connection_candidate_for_network_end_to_end() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = exec.run_singlethreaded(test_setup());
        let network_selector = test_values.network_selector;

        // create identifiers
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let bss_desc_1 = generate_random_bss_desc();

        // insert saved networks
        exec.run_singlethreaded(
            test_values.saved_network_manager.store(test_id_1.clone().into(), credential_1.clone()),
        )
        .unwrap();

        // get the sme proxy
        let mut iface_manager_inner = exec.run_singlethreaded(test_values.iface_manager.lock());
        let sme_proxy =
            exec.run_singlethreaded(iface_manager_inner.get_sme_proxy_for_scan()).unwrap();
        drop(iface_manager_inner);

        // Kick off network selection
        let network_selection_fut =
            network_selector.find_connection_candidate_for_network(sme_proxy, test_id_1.clone());
        pin_mut!(network_selection_fut);
        assert_variant!(exec.run_until_stalled(&mut network_selection_fut), Poll::Pending);

        // Check that a scan request was sent to the sme and send back results
        let expected_scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
            ssids: vec![test_id_1.ssid.clone()],
            channels: vec![],
        });
        let mock_scan_results = vec![
            fidl_sme::BssInfo {
                bssid: [0, 0, 0, 0, 0, 0],
                ssid: test_id_1.ssid.clone(),
                rssi_dbm: 10,
                snr_db: 10,
                channel: fidl_common::WlanChan {
                    primary: 1,
                    cbw: fidl_common::Cbw::Cbw20,
                    secondary80: 0,
                },
                // This network is WPA3, but should still match against the desired WPA2 network
                protection: fidl_sme::Protection::Wpa3Personal,
                compatible: true,
                bss_desc: bss_desc_1.clone(),
            },
            fidl_sme::BssInfo {
                bssid: [0, 0, 0, 0, 0, 0],
                ssid: "other ssid".as_bytes().to_vec(),
                rssi_dbm: 0,
                snr_db: 0,
                channel: fidl_common::WlanChan {
                    primary: 1,
                    cbw: fidl_common::Cbw::Cbw20,
                    secondary80: 0,
                },
                protection: fidl_sme::Protection::Wpa1,
                compatible: true,
                bss_desc: generate_random_bss_desc(),
            },
        ];
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut test_values.sme_stream,
            &expected_scan_request,
            mock_scan_results,
        );

        // Check that we pick a network
        let results = exec.run_singlethreaded(&mut network_selection_fut);
        assert_eq!(
            results,
            Some(types::ConnectionCandidate {
                network: test_id_1.clone(),
                credential: credential_1.clone(),
                bss: bss_desc_1,
                // This code path can't know if the network would have been observed in a passive
                // scan, since it never performs a passive scan.
                observed_in_passive_scan: None,
            })
        );
    }

    #[test]
    fn find_connection_candidate_for_network_end_to_end_with_failure() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = exec.run_singlethreaded(test_setup());
        let network_selector = test_values.network_selector;

        // create identifiers
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };

        // get the sme proxy
        let mut iface_manager_inner = exec.run_singlethreaded(test_values.iface_manager.lock());
        let sme_proxy =
            exec.run_singlethreaded(iface_manager_inner.get_sme_proxy_for_scan()).unwrap();
        drop(iface_manager_inner);

        // Kick off network selection
        let network_selection_fut =
            network_selector.find_connection_candidate_for_network(sme_proxy, test_id_1);
        pin_mut!(network_selection_fut);
        assert_variant!(exec.run_until_stalled(&mut network_selection_fut), Poll::Pending);

        // Return an error on the scan
        assert_variant!(
            exec.run_until_stalled(&mut test_values.sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                txn, req: _, control_handle: _
            }))) => {
                // Send failed scan response.
                let (_stream, ctrl) = txn
                    .into_stream_and_control_handle().expect("error accessing control handle");
                ctrl.send_on_error(&mut fidl_sme::ScanError {
                    code: fidl_sme::ScanErrorCode::InternalError,
                    message: "Failed to scan".to_string()
                }).expect("failed to send scan error");
            }
        );

        // Check that nothing is returned
        let results = exec.run_singlethreaded(&mut network_selection_fut);
        assert_eq!(results, None);
    }

    fn generate_random_bss() -> types::Bss {
        let mut rng = rand::thread_rng();
        let bss = (0..6).map(|_| rng.gen::<u8>()).collect::<Vec<u8>>();
        types::Bss {
            bssid: bss.as_slice().try_into().unwrap(),
            rssi: rng.gen_range(-100, 20),
            frequency: rng.gen_range(2000, 6000),
            channel: generate_random_channel(),
            timestamp_nanos: 0,
            snr_db: rng.gen_range(-20, 50),
            observed_in_passive_scan: rng.gen::<bool>(),
            compatible: rng.gen::<bool>(),
            bss_desc: generate_random_bss_desc(),
        }
    }

    fn generate_random_scan_result() -> types::ScanResult {
        let mut rng = rand::thread_rng();
        types::ScanResult {
            id: types::NetworkIdentifier {
                ssid: format!("scan result rand {}", rng.gen::<i32>()).as_bytes().to_vec(),
                type_: types::SecurityType::Wpa,
            },
            entries: vec![generate_random_bss(), generate_random_bss()],
            compatibility: types::Compatibility::Supported,
        }
    }

    fn generate_random_saved_network() -> (types::NetworkIdentifier, InternalSavedNetworkData) {
        let mut rng = rand::thread_rng();
        (
            types::NetworkIdentifier {
                ssid: format!("saved network rand {}", rng.gen::<i32>()).as_bytes().to_vec(),
                type_: types::SecurityType::Wpa,
            },
            InternalSavedNetworkData {
                credential: Credential::Password(
                    format!("password {}", rng.gen::<i32>()).as_bytes().to_vec(),
                ),
                has_ever_connected: false,
                recent_failure_count: 0,
            },
        )
    }

    #[fasync::run_singlethreaded(test)]
    async fn recorded_metrics_on_scan() {
        let (mut cobalt_api, mut cobalt_events) = create_mock_cobalt_sender_and_receiver();

        // create some identifiers
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };

        let mock_scan_results = vec![
            types::ScanResult {
                id: test_id_1.clone(),
                entries: vec![
                    types::Bss { observed_in_passive_scan: true, ..generate_random_bss() },
                    types::Bss { observed_in_passive_scan: true, ..generate_random_bss() },
                    types::Bss { observed_in_passive_scan: false, ..generate_random_bss() },
                ],
                compatibility: types::Compatibility::Supported,
            },
            types::ScanResult {
                id: test_id_2.clone(),
                entries: vec![types::Bss {
                    observed_in_passive_scan: true,
                    ..generate_random_bss()
                }],
                compatibility: types::Compatibility::Supported,
            },
            generate_random_scan_result(),
            generate_random_scan_result(),
            generate_random_scan_result(),
            generate_random_scan_result(),
            generate_random_scan_result(),
        ];

        let mut mock_saved_networks = HashMap::new();
        mock_saved_networks.insert(
            test_id_1.clone(),
            InternalSavedNetworkData {
                credential: Credential::Password("foo_pass".as_bytes().to_vec()),
                has_ever_connected: false,
                recent_failure_count: 0,
            },
        );
        mock_saved_networks.insert(
            test_id_2.clone(),
            InternalSavedNetworkData {
                credential: Credential::Password("bar_pass".as_bytes().to_vec()),
                has_ever_connected: false,
                recent_failure_count: 0,
            },
        );
        let random_saved_net = generate_random_saved_network();
        mock_saved_networks.insert(random_saved_net.0, random_saved_net.1);
        let random_saved_net = generate_random_saved_network();
        mock_saved_networks.insert(random_saved_net.0, random_saved_net.1);
        let random_saved_net = generate_random_saved_network();
        mock_saved_networks.insert(random_saved_net.0, random_saved_net.1);

        record_metrics_on_scan(&mock_scan_results, mock_saved_networks, &mut cobalt_api);

        // Three BSSs present for network 1 in scan results
        assert_eq!(
            cobalt_events.try_next().unwrap(),
            Some(
                CobaltEvent::builder(SAVED_NETWORK_IN_SCAN_RESULT_METRIC_ID)
                    .with_event_code(
                        SavedNetworkInScanResultMetricDimensionBssCount::TwoToFour.as_event_code()
                    )
                    .as_event()
            )
        );
        // One BSS present for network 2 in scan results
        assert_eq!(
            cobalt_events.try_next().unwrap(),
            Some(
                CobaltEvent::builder(SAVED_NETWORK_IN_SCAN_RESULT_METRIC_ID)
                    .with_event_code(
                        SavedNetworkInScanResultMetricDimensionBssCount::One.as_event_code()
                    )
                    .as_event()
            )
        );
        // Total of two saved networks in the scan results
        assert_eq!(
            cobalt_events.try_next().unwrap(),
            Some(
                CobaltEvent::builder(SCAN_RESULTS_RECEIVED_METRIC_ID)
                    .with_event_code(
                        ScanResultsReceivedMetricDimensionSavedNetworksCount::TwoToFour
                            .as_event_code()
                    )
                    .as_event()
            )
        );
        // One saved networks that was discovered via active scan
        assert_eq!(
            cobalt_events.try_next().unwrap(),
            Some(
                CobaltEvent::builder(SAVED_NETWORK_IN_SCAN_RESULT_WITH_ACTIVE_SCAN_METRIC_ID)
                    .with_event_code(ActiveScanSsidsObserved::One.as_event_code())
                    .as_event()
            )
        );
        // No more metrics
        assert!(cobalt_events.try_next().is_err());
    }

    #[fasync::run_singlethreaded(test)]
    async fn recorded_metrics_on_scan_no_saved_networks() {
        let (mut cobalt_api, mut cobalt_events) = create_mock_cobalt_sender_and_receiver();

        let mock_scan_results = vec![
            generate_random_scan_result(),
            generate_random_scan_result(),
            generate_random_scan_result(),
            generate_random_scan_result(),
            generate_random_scan_result(),
        ];

        let mock_saved_networks = HashMap::new();

        record_metrics_on_scan(&mock_scan_results, mock_saved_networks, &mut cobalt_api);

        // No saved networks in scan results
        assert_eq!(
            cobalt_events.try_next().unwrap(),
            Some(
                CobaltEvent::builder(SCAN_RESULTS_RECEIVED_METRIC_ID)
                    .with_event_code(
                        ScanResultsReceivedMetricDimensionSavedNetworksCount::Zero.as_event_code()
                    )
                    .as_event()
            )
        );
        // Also no saved networks that were discovered via active scan
        assert_eq!(
            cobalt_events.try_next().unwrap(),
            Some(
                CobaltEvent::builder(SAVED_NETWORK_IN_SCAN_RESULT_WITH_ACTIVE_SCAN_METRIC_ID)
                    .with_event_code(ActiveScanSsidsObserved::Zero.as_event_code())
                    .as_event()
            )
        );
        // No more metrics
        assert!(cobalt_events.try_next().is_err());
    }
}
