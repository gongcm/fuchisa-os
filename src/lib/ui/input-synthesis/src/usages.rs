// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// Standard [USB HID] usages.
///
/// [USB HID]: https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Usages {
    HidUsageKeyErrorRollover = 0x01,
    HidUsageKeyPostFail = 0x02,
    HidUsageKeyErrorUndef = 0x03,
    HidUsageKeyA = 0x04,
    HidUsageKeyB = 0x05,
    HidUsageKeyC = 0x06,
    HidUsageKeyD = 0x07,
    HidUsageKeyE = 0x08,
    HidUsageKeyF = 0x09,
    HidUsageKeyG = 0x0A,
    HidUsageKeyH = 0x0B,
    HidUsageKeyI = 0x0C,
    HidUsageKeyJ = 0x0D,
    HidUsageKeyK = 0x0E,
    HidUsageKeyL = 0x0F,
    HidUsageKeyM = 0x10,
    HidUsageKeyN = 0x11,
    HidUsageKeyO = 0x12,
    HidUsageKeyP = 0x13,
    HidUsageKeyQ = 0x14,
    HidUsageKeyR = 0x15,
    HidUsageKeyS = 0x16,
    HidUsageKeyT = 0x17,
    HidUsageKeyU = 0x18,
    HidUsageKeyV = 0x19,
    HidUsageKeyW = 0x1A,
    HidUsageKeyX = 0x1B,
    HidUsageKeyY = 0x1C,
    HidUsageKeyZ = 0x1D,
    HidUsageKey1 = 0x1E,
    HidUsageKey2 = 0x1F,
    HidUsageKey3 = 0x20,
    HidUsageKey4 = 0x21,
    HidUsageKey5 = 0x22,
    HidUsageKey6 = 0x23,
    HidUsageKey7 = 0x24,
    HidUsageKey8 = 0x25,
    HidUsageKey9 = 0x26,
    HidUsageKey0 = 0x27,
    HidUsageKeyEnter = 0x28,
    HidUsageKeyEsc = 0x29,
    HidUsageKeyBackspace = 0x2A,
    HidUsageKeyTab = 0x2B,
    HidUsageKeySpace = 0x2C,
    HidUsageKeyMinus = 0x2D,
    HidUsageKeyEqual = 0x2E,
    HidUsageKeyLeftbrace = 0x2F,
    HidUsageKeyRightbrace = 0x30,
    HidUsageKeyBackslash = 0x31,
    HidUsageKeyNonUsOctothorpe = 0x32,
    HidUsageKeySemicolon = 0x33,
    HidUsageKeyApostrophe = 0x34,
    HidUsageKeyGrave = 0x35,
    HidUsageKeyComma = 0x36,
    HidUsageKeyDot = 0x37,
    HidUsageKeySlash = 0x38,
    HidUsageKeyCapslock = 0x39,
    HidUsageKeyF1 = 0x3A,
    HidUsageKeyF2 = 0x3B,
    HidUsageKeyF3 = 0x3C,
    HidUsageKeyF4 = 0x3D,
    HidUsageKeyF5 = 0x3E,
    HidUsageKeyF6 = 0x3F,
    HidUsageKeyF7 = 0x40,
    HidUsageKeyF8 = 0x41,
    HidUsageKeyF9 = 0x42,
    HidUsageKeyF10 = 0x43,
    HidUsageKeyF11 = 0x44,
    HidUsageKeyF12 = 0x45,
    HidUsageKeyPrintscreen = 0x46,
    HidUsageKeyScrolllock = 0x47,
    HidUsageKeyPause = 0x48,
    HidUsageKeyInsert = 0x49,
    HidUsageKeyHome = 0x4A,
    HidUsageKeyPageup = 0x4B,
    HidUsageKeyDelete = 0x4C,
    HidUsageKeyEnd = 0x4D,
    HidUsageKeyPagedown = 0x4E,
    HidUsageKeyRight = 0x4F,
    HidUsageKeyLeft = 0x50,
    HidUsageKeyDown = 0x51,
    HidUsageKeyUp = 0x52,
    HidUsageKeyNumlock = 0x53,
    HidUsageKeyKpSlash = 0x54,
    HidUsageKeyKpAsterisk = 0x55,
    HidUsageKeyKpMinus = 0x56,
    HidUsageKeyKpPlus = 0x57,
    HidUsageKeyKpEnter = 0x58,
    HidUsageKeyKp1 = 0x59,
    HidUsageKeyKp2 = 0x5A,
    HidUsageKeyKp3 = 0x5B,
    HidUsageKeyKp4 = 0x5C,
    HidUsageKeyKp5 = 0x5D,
    HidUsageKeyKp6 = 0x5E,
    HidUsageKeyKp7 = 0x5F,
    HidUsageKeyKp8 = 0x60,
    HidUsageKeyKp9 = 0x61,
    HidUsageKeyKp0 = 0x62,
    HidUsageKeyKpDot = 0x63,
    HidUsageKeyNonUsBackslash = 0x64,
    HidUsageKeyLeftCtrl = 0xE0,
    HidUsageKeyLeftShift = 0xE1,
    HidUsageKeyLeftAlt = 0xE2,
    HidUsageKeyLeftGui = 0xE3,
    HidUsageKeyRightCtrl = 0xE4,
    HidUsageKeyRightShift = 0xE5,
    HidUsageKeyRightAlt = 0xE6,
    HidUsageKeyRightGui = 0xE7,
    // TODO: The following two values are incorrect and are not actually USB HID codes, but are
    //       currently the values that appear in hid/usages.h. Eventually we will want to migrate to
    //       fuchsia.ui.input.Key.
    HidUsageKeyVolUp = 0xE8,
    HidUsageKeyVolDown = 0xE9,
}
