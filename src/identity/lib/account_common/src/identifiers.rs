// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The fuchsia.auth.account interface uses a common pattern of wrapped structs to provide type
// safety for the various integer identifiers. The FIDL bindings for these types are currently not
// very ergonomic since all FIDL structs potentially can grow to contain handles so prohibit
// useful features like cloning. This module provides convenient and full featured alternatives for
// the wrapped structs that implement From/Into for each conversion to the FIDL type.
//
// Longer term we aspire to annotate this wrapper pattern in FIDL, which would allow the bindings
// to be more ergonomic and remove the need for this module.

use serde::de::{Deserialize, Deserializer};
use serde::Serialize;
use std::cmp::Ordering;
use std::fmt::{Debug, Formatter};
use std::hash::{Hash, Hasher};

/// Implements `$name` as a new wrapper type.
///
/// This wrapper type implements many standard copy and comparison traits and provides conversion
/// in both directions to a type with the same name in `$fidl_crate`. This type must by a struct
/// contain a single field of type `$type` named 'id'.
///
/// `$type` must implement the following traits: `Debug`, `Clone`, `Hash`, `Eq`, and `Ord`.
///
/// # Examples
///
/// ```
/// wrapper_type!(LocalAccountId, u64, fidl_fuchsia_auth_account);
/// ```
macro_rules! wrapper_type {
    ($name:ident, $type:ty, $fidl_crate:ident) => {
        /// A simpler wrapper around the less ergonomic autogenerated FIDL type.
        pub struct $name {
            inner: $fidl_crate::$name,
        }

        impl $name {
            /// Construct a new instance from the supplied id.
            pub fn new(id: $type) -> Self {
                $name { inner: $fidl_crate::$name { id: id } }
            }
        }

        impl Clone for $name {
            fn clone(&self) -> $name {
                $name { inner: $fidl_crate::$name { id: self.inner.id.clone() } }
            }
        }

        impl Hash for $name {
            fn hash<H: Hasher>(&self, state: &mut H) {
                self.inner.id.hash(state);
            }
        }

        impl Ord for $name {
            fn cmp(&self, other: &$name) -> Ordering {
                self.inner.id.cmp(&other.inner.id)
            }
        }

        impl PartialOrd for $name {
            fn partial_cmp(&self, other: &$name) -> Option<Ordering> {
                Some(self.cmp(other))
            }
        }

        impl PartialEq for $name {
            fn eq(&self, other: &$name) -> bool {
                self.inner.id == other.inner.id
            }
        }

        impl Eq for $name {}

        impl Debug for $name {
            fn fmt(&self, f: &mut Formatter) -> std::fmt::Result {
                write!(f, "{} {{ id: {:?} }}", stringify!($name), self.inner.id)
            }
        }

        impl From<$fidl_crate::$name> for $name {
            fn from(fidl_type: $fidl_crate::$name) -> Self {
                $name { inner: fidl_type }
            }
        }

        impl From<$name> for $fidl_crate::$name {
            fn from(wrapper: $name) -> Self {
                wrapper.inner
            }
        }

        impl From<$name> for $type {
            fn from(wrapper: $name) -> Self {
                wrapper.inner.id
            }
        }

        impl AsRef<$fidl_crate::$name> for $name {
            fn as_ref(&self) -> &$fidl_crate::$name {
                &self.inner
            }
        }

        impl AsMut<$fidl_crate::$name> for $name {
            fn as_mut(&mut self) -> &mut $fidl_crate::$name {
                &mut self.inner
            }
        }

        impl Serialize for $name {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: ::serde::Serializer,
            {
                Serialize::serialize(&self.inner.id, serializer)
            }
        }

        impl<'a> Deserialize<'a> for $name {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: Deserializer<'a>,
            {
                let id = Deserialize::deserialize(deserializer)?;
                Ok($name { inner: $fidl_crate::$name { id } })
            }
        }
    };
}

wrapper_type!(LocalAccountId, u64, fidl_fuchsia_auth_account);
/// Convenience type for the autogenerated FIDL LocalAccountId.
pub type FidlLocalAccountId = fidl_fuchsia_auth_account::LocalAccountId;

wrapper_type!(LocalPersonaId, u64, fidl_fuchsia_auth_account);
/// Convenience type for the autogenerated FIDL LocalPersonaId.
pub type FidlLocalPersonaId = fidl_fuchsia_auth_account::LocalPersonaId;

wrapper_type!(GlobalAccountId, Vec<u8>, fidl_fuchsia_auth_account);
/// Convenience type for the autogenerated FIDL GlobalAccountId.
pub type FidlGlobalAccountId = fidl_fuchsia_auth_account::GlobalAccountId;

impl LocalAccountId {
    /// A string representing the account in a canonical way, safe for file/directory names
    pub fn to_canonical_string(&self) -> String {
        self.inner.id.to_string()
    }
}

impl LocalPersonaId {
    /// A string representing the persona in a canonical way, safe for file/directory names
    pub fn to_canonical_string(&self) -> String {
        self.inner.id.to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Verify a few simple properties on one of our generated types to verify the macros are
    // working. Since the generation of these types is automated we only test one.

    #[test]
    fn test_clonable() {
        let id: u64 = 456;
        let value = LocalAccountId::new(id);
        let cloned = value.clone();
        assert_eq!(id, value.inner.id);
        assert_eq!(id, cloned.inner.id);
    }

    #[test]
    fn test_debug() {
        let value = LocalAccountId::new(1111);
        assert_eq!("LocalAccountId { id: 1111 }", format!("{:?}", value));
    }

    #[test]
    fn test_eq() {
        let val_1 = LocalAccountId::new(4231);
        let val_2 = LocalAccountId::new(4231);
        assert_eq!(val_1, val_2);
    }

    #[test]
    fn test_ne() {
        let val_1 = LocalAccountId::new(554);
        let val_2 = LocalAccountId::new(555);
        assert_ne!(val_1, val_2);
    }

    #[test]
    fn test_ord() {
        let val_1 = LocalAccountId::new(333);
        let val_2 = LocalAccountId::new(444);
        assert!(val_1 < val_2);
    }

    #[test]
    fn test_to_primitive() {
        let value = LocalAccountId::new(333);
        assert_eq!(u64::from(value), 333u64);
    }
}
