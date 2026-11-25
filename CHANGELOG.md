# Changelog

All notable changes to CYD//RECON are recorded here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- **Settings screen** (gear button by the filter tabs), persisted to NVS:
  master sound + volume, touch clicks, tracker Geiger, per-alert toggles
  (chirp / pop-up / LED flash), low-battery warning, backlight brightness and
  status-LED behaviour, plus TEST ALERT and RESET DEFAULTS. Volume maps evenly
  in dB across the slider.
- **LAN device names in network mode.** Each discovered host is resolved via
  unicast mDNS reverse lookup, then reverse DNS through the router, then
  NetBIOS. The host detail screen shows the name and where it came from. The
  board also syncs its clock over NTP once joined (used for capture timestamps).
- **WiFi packet capture** (RF SCOPE → REC PCAP). Writes 802.11 management/data
  frames to a FAT32 microSD card as `/pcap/cap_NNNN.pcap` with a radiotap
  header (channel + signal), openable in Wireshark. Follows channel hopping or
  a locked/joined channel; buffered through PSRAM; files roll at 100 MB; a REC
  badge and live stats show while recording. Control frames and BLE are not
  captured; encrypted traffic stays encrypted.
- **SSH posture check** (host detail → CHECK SSH, shown when port 22 is open).
  Reads the SSH version banner and audits the server's offered key-exchange,
  host-key, cipher and MAC algorithms from its KEXINIT, flagging legacy/weak
  ones (ssh-rsa/ssh-dss host keys, SHA-1 / group1 kex, CBC/arcfour/3DES
  ciphers, MD5/SHA-1/-96 MACs). Passive: it never sends a username or password
  and never attempts to authenticate.
- **Power off / wake on the BOOT button.** A BOOT press puts the device into
  deep sleep (radios, display and capture shut down cleanly); the next BOOT
  press wakes it with a fresh boot. Touch-test is now reached from Settings and
  the touch-test screen has an on-screen BACK.

### Fixed
- **Touch latched on the first touch.** The ST77922 controller holds its report
  until the whole point block is read; we read only point 0, so after one touch
  it reported the same point forever and no taps registered. We now read the
  advertised number of touch slots in a single transaction.
