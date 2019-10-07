// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::{Error, ResultExt};
use log::info;
use omaha_client::{
    common::{App, AppSet, UserCounting, Version},
    configuration::{Config, Updater},
    protocol::{request::OS, Cohort},
};
use std::fs;
use std::io;

pub fn get_app_set(version: &str, default_channel: Option<String>) -> Result<AppSet, Error> {
    let id = fs::read_to_string("/config/data/omaha_app_id")?;
    let version = version
        .parse::<Version>()
        .context(format!("Unable to parse '{}' as Omaha version format", version))?;
    let channel_config = sysconfig_client::channel::read_channel_config();
    info!("Channel configuration in sysconfig: {:?}", channel_config);
    let channel =
        channel_config.map(|config| config.channel_name().to_string()).ok().or(default_channel);
    let cohort = Cohort { hint: channel.clone(), name: channel, ..Cohort::default() };
    // Fuchsia only has a single app.
    Ok(AppSet::new(vec![App {
        id,
        version,
        fingerprint: None,
        cohort,
        user_counting: UserCounting::ClientRegulatedByDate(None),
    }]))
}

pub fn get_config(version: &str) -> Config {
    Config {
        updater: Updater { name: "Fuchsia".to_string(), version: Version::from([0, 0, 1, 0]) },

        os: OS {
            platform: "Fuchsia".to_string(),
            version: version.to_string(),
            service_pack: "".to_string(),
            arch: std::env::consts::ARCH.to_string(),
        },

        service_url: "https://clients2.google.com/service/update2/fuchsia/json".to_string(),
    }
}

pub fn get_version() -> Result<String, io::Error> {
    fs::read_to_string("/config/build-info/version").map(|s| s.trim_end().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use fuchsia_async as fasync;

    #[test]
    fn test_get_config() {
        let config = get_config("1.2.3.4");
        assert_eq!(config.updater.name, "Fuchsia");
        let os = config.os;
        assert_eq!(os.platform, "Fuchsia");
        assert_eq!(os.version, "1.2.3.4");
        assert_eq!(os.arch, std::env::consts::ARCH);
        assert_eq!(config.service_url, "https://clients2.google.com/service/update2/fuchsia/json");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_app_set() {
        let app_set = get_app_set("1.2.3.4", None).unwrap();
        let apps = app_set.to_vec().await;
        assert_eq!(apps.len(), 1);
        assert_eq!(apps[0].id, "fuchsia:test-app-id");
        assert_eq!(apps[0].version, Version::from([1, 2, 3, 4]));
        assert_eq!(apps[0].cohort.name, None);
        assert_eq!(apps[0].cohort.hint, None);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_app_set_default_channel() {
        let app_set = get_app_set("1.2.3.4", Some("default-channel".to_string())).unwrap();
        let apps = app_set.to_vec().await;
        assert_eq!(apps.len(), 1);
        assert_eq!(apps[0].id, "fuchsia:test-app-id");
        assert_eq!(apps[0].version, Version::from([1, 2, 3, 4]));
        assert_eq!(apps[0].cohort.name, Some("default-channel".to_string()));
        assert_eq!(apps[0].cohort.hint, Some("default-channel".to_string()));
    }

    #[test]
    fn test_get_app_set_invalid_version() {
        assert!(get_app_set("invalid version", None).is_err());
    }
}
